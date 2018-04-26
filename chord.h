#ifndef CHORD_H
#define CHORD_H

#include <stdbool.h>
#define NUMBER_OF_BITS 8

typedef struct{
	int nodeId;
	
	int predecessor;
	int predecessorPort;

	int successor;

	int fingerTable[NUMBER_OF_BITS];
	int fingerTablePorts[NUMBER_OF_BITS];
	bool keys[256];

}Node;

#endif
