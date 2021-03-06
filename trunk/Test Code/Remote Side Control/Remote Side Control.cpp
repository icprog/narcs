#include <afxwin.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <iostream>
#include <MMSystem.h>
#include <math.h>
#include "DataTransferStructs.h"


#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#import "C:\Program Files\CRS Robotics\ActiveRobot\ActiveRobot.dll"

#define DEFAULT_PORT  "62009"
#define ARM_SPEED  100
#define POSITION_UPDATE_WAIT_INTERVAL_MS  400
#define POSITION_SCALNG_FACTOR  0.75
//#define MAX_ARM_REACH_MM  650
#define END_EFFECTOR_LENGTH_MM  153
#define END_EFFECTOR_SUPPORT_LINK_LENGTH_MM  52
//#define NEG_ANGLE_GROUND_DETECTION_OFFSET_MM  28
#define Z_MIN_MM  50
#define PI  3.14159265
#define FORCE_UPDATE_REQUEST_CHAR  'f'
#define START_VIBRATING_MOTORS_CHAR  'o'
#define STOP_VIBRATING_MOTORS_CHAR  'r'



using namespace ACTIVEROBOTLib;
using namespace std;


int main() {
	int retCode = 0;

	if FAILED(CoInitialize(NULL))
	{
		AfxMessageBox("CoInitialize() failed.");
		exit(1);
	}

	ICRSRobotPtr Robot =
					ICRSRobotPtr(__uuidof(CRSRobot));
	try
	{
		Robot->ControlGet();
		Robot->PutSpeed(ARM_SPEED);
		Robot->PutBlendMotion(-1);
		Robot->Ready();
		Robot->Finish(ftTight);
		Robot->GripperOpen(30);
		Robot->GripperFinish();
	}
	catch (_com_error MyError)
	{
		char WorkString[255];
		sprintf(WorkString, "The following error occurred during initialization --\n%s", (LPCTSTR) MyError.Description());
		AfxMessageBox(WorkString);
		Robot->ControlRelease();
		exit(1);
	}

	ICRSLocationPtr readyPosition = Robot->GetWorldLocation(
														ptActual);
	const float readyPositionX = readyPosition->Getx(),
		        readyPositionY = readyPosition->Gety(),
		        readyPositionZ = readyPosition->Getz();


	ICRSLocationPtr endEffectorPosition = 
							ICRSLocationPtr(__uuidof(CRSLocation));

	try
	{
		endEffectorPosition->PutFlags(readyPosition->GetFlags());
		endEffectorPosition->PutClass(readyPosition->GetClass());
		endEffectorPosition->PutRobotType(locRCA255);
	}
	catch (_com_error MyError)
	{
		char WorkString[255];
		sprintf(WorkString, "The following error occurred during initialization --\n%s", (LPCTSTR) MyError.Description());
		AfxMessageBox(WorkString);
		Robot->ControlRelease();
		exit(1);
	}


    int iResult, iSendResult;
	WSADATA wsaData;	//to inialize Windows socket dll
	struct addrinfo *result = NULL, hints;
	fd_set master_socket_descriptor_set,
		   request_socket_descriptor_set;
	SOCKET listenSocket = INVALID_SOCKET,
		   kinectAndIMUSocket = INVALID_SOCKET,
		   arduinoSocket = INVALID_SOCKET,
		   maxSocket = INVALID_SOCKET;

	//Initialize Winscok
	iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (iResult) {
		printf("WSAStartup failed: %d\n", iResult);
		goto FAILURE;
	}

	//Creating socket IP address
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	//resolve the local address and port to be used by the server
	iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
	if (iResult) {
		printf("getaddrinfofailed: %d\n", iResult);
		goto FAILURE;
	}
	
	//Create a SOCKET for the server to listen for client connections
	listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (listenSocket == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		goto FAILURE;
	}
	

	//Setup the TCP listening socket
	iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		printf("bind failed with error: %d\n", WSAGetLastError());
		goto FAILURE;
	}

	if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		printf("Listen failed with error: %ld\n", WSAGetLastError());
		goto FAILURE;
	}

	kinectAndIMUSocket = accept(listenSocket, NULL, NULL);
	if (kinectAndIMUSocket == INVALID_SOCKET) {
		printf("accept failed: %d\n", WSAGetLastError());
		goto FAILURE;
	}

	arduinoSocket = accept(listenSocket, NULL, NULL);
	if (arduinoSocket == INVALID_SOCKET) {
		printf("accept failed: %d\n", WSAGetLastError());
		goto FAILURE;
	}

	freeaddrinfo(result);
	result = NULL;
		

	FD_ZERO(&master_socket_descriptor_set);
	FD_SET(kinectAndIMUSocket, &master_socket_descriptor_set);
	FD_SET(arduinoSocket, &master_socket_descriptor_set);
	
	if(kinectAndIMUSocket > arduinoSocket)
	{
		maxSocket = kinectAndIMUSocket;
	}
	else
	{
		maxSocket = arduinoSocket;
	}
	
	KinectAndHandOrientation kinectAndHandOrientation;
	char sendBuffer[1];
	int curr_bytes_read = -1;
	unsigned int total_bytes_read = 0;
	char *curr_buffer_ptr = sendBuffer;
	float endEffectorX,
		  endEffectorY,
		  endEffectorZ,
		  endEffectorZMin = 0;
	unsigned int totalBytesWritten = 0;
	int currBytesWritten = -1;
	int currForceValue = 0,
		prevForceValue = 0;
	const float gripperForceDistanceMapping[5] = 
										{1.58, 1.32, 1.08, 0.87, 0.73};
	bool vibratorMotorsOn = false;


	// Send first Kinect and IMU update request
	curr_buffer_ptr = sendBuffer;
	totalBytesWritten = 0;
	currBytesWritten = -1;
	while(totalBytesWritten < 1)
	{
		currBytesWritten = send(kinectAndIMUSocket,
								curr_buffer_ptr,
								1 - totalBytesWritten,
								0);
		if (currBytesWritten == 0)
		{
			printf("Connection closing...\n");
			closesocket(kinectAndIMUSocket);
			WSACleanup();
			Robot->ControlRelease();
			cin.get();
			return 0;
		}
		else if (currBytesWritten < 0)
		{
			printf("send failed: %d\n", WSAGetLastError());
			closesocket(kinectAndIMUSocket);
			WSACleanup();
			Robot->ControlRelease();
			cin.get();
			return -1;
		}

		totalBytesWritten += currBytesWritten;
		curr_buffer_ptr += currBytesWritten;
	}


	// Send first force update request
	sendBuffer[0] = FORCE_UPDATE_REQUEST_CHAR;
	curr_buffer_ptr = sendBuffer;
	totalBytesWritten = 0;
	currBytesWritten = -1;
	while(totalBytesWritten < 1)
	{
		currBytesWritten = send(arduinoSocket,
								curr_buffer_ptr,
								1 - totalBytesWritten,
								0);
		if (currBytesWritten == 0)
		{
			goto FAILURE;
		}
		else if (currBytesWritten < 0)
		{
			goto FAILURE;
		}

		totalBytesWritten += currBytesWritten;
		curr_buffer_ptr += currBytesWritten;
	}
	

	while(true)
	{
		request_socket_descriptor_set = master_socket_descriptor_set;
	
		
		if(select(maxSocket + 1,
				  &request_socket_descriptor_set,
				  NULL,
				  NULL,
				  NULL) < 0)
		{
			printf("select() error");
			goto FAILURE;
		}

		if(FD_ISSET(kinectAndIMUSocket,
					&request_socket_descriptor_set))
		{
			// Receive Kinect and IMU update
			curr_bytes_read = -1;
			total_bytes_read = 0;
			curr_buffer_ptr = (char *)(&kinectAndHandOrientation);
			while(total_bytes_read < sizeof(kinectAndHandOrientation))
			{
				curr_bytes_read = recv(kinectAndIMUSocket,
										curr_buffer_ptr,
										sizeof(kinectAndHandOrientation) - total_bytes_read,
										0);
				if (curr_bytes_read == 0)
				{
					printf("Connection closing...\n");
					closesocket(kinectAndIMUSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return 0;
				}
				else if (curr_bytes_read < 0)
				{
					printf("recv failed: %d\n", WSAGetLastError());
					closesocket(kinectAndIMUSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return -1;
				}

				total_bytes_read += curr_bytes_read;
				curr_buffer_ptr += curr_bytes_read;
			}

			/*
			memcpy_s( (void *)(&endEffectorX),
						sizeof(endEffectorX),
						(const void *)(position_update_buffer + 8),
						4);
			memcpy_s( (void *)(&endEffectorY),
						sizeof(endEffectorY),
						(const void *)(position_update_buffer),
						4);
			memcpy_s( (void *)(&endEffectorZ),
						sizeof(endEffectorZ),
						(const void *)(position_update_buffer + 4),
						4);
			*/

			endEffectorX = kinectAndHandOrientation.kinectData.rightHandZ * 
							POSITION_SCALNG_FACTOR +
							readyPositionX;
			endEffectorY = -1 * kinectAndHandOrientation.kinectData.rightHandX *
							POSITION_SCALNG_FACTOR +
							readyPositionY;
			endEffectorZ = kinectAndHandOrientation.kinectData.rightHandY *
							POSITION_SCALNG_FACTOR +
							readyPositionZ;

			// <Ahmed> 
			//endEffectorPosition->Putxrot(kinectAndHandOrientation.HandOrientation.roll);
			//endEffectorPosition->Putyrot(kinectAndHandOrientation.HandOrientation.pitch);
			
			//cout << "Roll = " << kinectAndHandOrientation.HandOrientation.roll << endl
			//	 << "Pitch = " << kinectAndHandOrientation.HandOrientation.pitch << endl;
			// </Ahmed>

			if( kinectAndHandOrientation.HandOrientation.pitch > 0 )
			{
				endEffectorZMin = 
					endEffectorZ -
					( float )( END_EFFECTOR_LENGTH_MM ) *
					sin( kinectAndHandOrientation.HandOrientation.pitch *
							( ( float )( PI ) ) /
							( ( float )( 180 ) ) );
			}
			else if( kinectAndHandOrientation.HandOrientation.pitch < 0 )
			{
				endEffectorZMin = 
					endEffectorZ -
					( float )( END_EFFECTOR_SUPPORT_LINK_LENGTH_MM ) *
					sin( ( float )( -1 ) * kinectAndHandOrientation.HandOrientation.pitch *
							( ( float )( PI ) ) /
							( ( float )( 180 ) ) );
			}
			else
			{
				endEffectorZMin = endEffectorZ;
			}

	
			if( endEffectorZMin >= (float)(Z_MIN_MM) )
			{
				try
				{
					endEffectorPosition->Putx(endEffectorX);
					endEffectorPosition->Puty(endEffectorY);
					endEffectorPosition->Putz(endEffectorZ);
					endEffectorPosition->Putxrot(
										kinectAndHandOrientation.HandOrientation.roll);
			        endEffectorPosition->Putyrot(
										kinectAndHandOrientation.HandOrientation.pitch);

					try
					{
						Robot->MoveStraight(endEffectorPosition);
						Robot->Finish(ftLoose);

						if(vibratorMotorsOn){
							sendBuffer[0] = STOP_VIBRATING_MOTORS_CHAR;
							curr_buffer_ptr = sendBuffer;
							totalBytesWritten = 0;
							currBytesWritten = -1;
							while(totalBytesWritten < 1)
							{
								currBytesWritten = send(arduinoSocket,
														curr_buffer_ptr,
														1 - totalBytesWritten,
														0);
								if (currBytesWritten == 0)
								{
									goto FAILURE;
								}
								else if (currBytesWritten < 0)
								{
									goto FAILURE;
								}

								totalBytesWritten += currBytesWritten;
								curr_buffer_ptr += currBytesWritten;
							}


							vibratorMotorsOn = false;
						}
					}
					catch(...)
					{
						printf("Trying to move to an invalid location!\n");

						if(!vibratorMotorsOn){
							sendBuffer[0] = START_VIBRATING_MOTORS_CHAR;
							curr_buffer_ptr = sendBuffer;
							totalBytesWritten = 0;
							currBytesWritten = -1;
							while(totalBytesWritten < 1)
							{
								currBytesWritten = send(arduinoSocket,
														curr_buffer_ptr,
														1 - totalBytesWritten,
														0);
								if (currBytesWritten == 0)
								{
									goto FAILURE;
								}
								else if (currBytesWritten < 0)
								{
									goto FAILURE;
								}

								totalBytesWritten += currBytesWritten;
								curr_buffer_ptr += currBytesWritten;
							}


							vibratorMotorsOn = true;
						}
					}
				}
				catch (_com_error MyError)
				{
					char WorkString[255];
					sprintf(WorkString, "The following error occurred during initialization --\n%s", (LPCTSTR) MyError.Description());
					AfxMessageBox(WorkString);
					closesocket(kinectAndIMUSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return -1;
				} // catch (_com_error MyError)
			}
			else
			{
				printf("Ground collision detected!\n");

				if(!vibratorMotorsOn){
					sendBuffer[0] = START_VIBRATING_MOTORS_CHAR;
					curr_buffer_ptr = sendBuffer;
					totalBytesWritten = 0;
					currBytesWritten = -1;
					while(totalBytesWritten < 1)
					{
						currBytesWritten = send(arduinoSocket,
												curr_buffer_ptr,
												1 - totalBytesWritten,
												0);
						if (currBytesWritten == 0)
						{
							goto FAILURE;
						}
						else if (currBytesWritten < 0)
						{
							goto FAILURE;
						}

						totalBytesWritten += currBytesWritten;
						curr_buffer_ptr += currBytesWritten;
					}


					vibratorMotorsOn = true;
				}
			}

			// send Kinect and IMU update request
			curr_buffer_ptr = sendBuffer;
			totalBytesWritten = 0;
			currBytesWritten = -1;
			while(totalBytesWritten < 1)
			{
				currBytesWritten = send(kinectAndIMUSocket,
										curr_buffer_ptr,
										1 - totalBytesWritten,
										0);
				if (currBytesWritten == 0)
				{
					printf("Connection closing...\n");
					closesocket(kinectAndIMUSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return 0;
				}
				else if (currBytesWritten < 0)
				{
					printf("send failed: %d\n", WSAGetLastError());
					closesocket(kinectAndIMUSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return -1;
				}

				totalBytesWritten += currBytesWritten;
				curr_buffer_ptr += currBytesWritten;
			}	
		} // if(FD_ISSET(kinectAndIMUSocket,
		 //			&request_socket_descriptor_set))
		if(FD_ISSET(arduinoSocket,
					&request_socket_descriptor_set))
		{
			// Receive Arduino Force Value
			curr_bytes_read = -1;
			total_bytes_read = 0;
			curr_buffer_ptr = (char *)(&currForceValue);
			while(total_bytes_read < sizeof(currForceValue))
			{
				curr_bytes_read = recv(arduinoSocket,
										curr_buffer_ptr,
										sizeof(currForceValue) - total_bytes_read,
										0);
				if (curr_bytes_read == 0)
				{
					printf("Connection closing...\n");
					closesocket(arduinoSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return 0;
				}
				else if (curr_bytes_read < 0)
				{
					printf("recv failed: %d\n", WSAGetLastError());
					closesocket(arduinoSocket);
					WSACleanup();
					Robot->ControlRelease();
					cin.get();
					return -1;
				}

				total_bytes_read += curr_bytes_read;
				curr_buffer_ptr += curr_bytes_read;
			}
			cout<<currForceValue<<endl;
			
			if( (currForceValue == 0) &&
				(prevForceValue != 0) )
			{
				Robot->GripperOpen(30);
				Robot->GripperFinish();
			}
			else if(currForceValue > prevForceValue)
			{
				Robot->GripperClose((float)(currForceValue));
				Robot->GripperFinish();
			}
			else if(currForceValue < prevForceValue)
			{
				int index = currForceValue/10;
				index -= 3;

				float lowerLimit = gripperForceDistanceMapping[index],
					  upperLimit = gripperForceDistanceMapping[index + 1];

				float interpolatedDistance = lowerLimit +
											 (upperLimit - lowerLimit)/((float)(10))*
											 ((float)(currForceValue - (index + 3)*10));
				
				Robot->PutGripperDistance(interpolatedDistance);
				Robot->GripperFinish();
			}


			prevForceValue = currForceValue;
			

			// Send force update request
			sendBuffer[0] = FORCE_UPDATE_REQUEST_CHAR;
			curr_buffer_ptr = sendBuffer;
			totalBytesWritten = 0;
			currBytesWritten = -1;
			while(totalBytesWritten < 1)
			{
				currBytesWritten = send(arduinoSocket,
										curr_buffer_ptr,
										1 - totalBytesWritten,
										0);
				if (currBytesWritten == 0)
				{
					goto FAILURE;
				}
				else if (currBytesWritten < 0)
				{
					goto FAILURE;
				}

				totalBytesWritten += currBytesWritten;
				curr_buffer_ptr += currBytesWritten;
			}
		} // if(FD_ISSET(arduinoSocket,
		  //             &request_socket_descriptor_set))
	} // while(true)


	retCode = 0;
	goto SUCCESS;

FAILURE:
	retCode = -1;

SUCCESS:
	
	Robot->ControlRelease();

	if(result != NULL)
	{
		freeaddrinfo(result);
	}
	if(listenSocket != INVALID_SOCKET)
	{
		closesocket(listenSocket);
	}
	if(kinectAndIMUSocket != INVALID_SOCKET)
	{
		closesocket(kinectAndIMUSocket);
	}
	if(arduinoSocket != INVALID_SOCKET)
	{
		closesocket(arduinoSocket);
	}
	WSACleanup();

	cin.get();
	return 0;
}
