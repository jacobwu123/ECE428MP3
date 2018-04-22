#ifndef CHORD_H
#define CHORD_H

#define NUMBER_OF_BITS 7

typedef struct{
	int nodeId;
	int predecessor;
	int successor;

	int fingerTable[NUMBER_OF_BITS];
	int keys[128];

}Node;

#endif
