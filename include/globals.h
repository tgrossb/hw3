#ifndef GLOBALS_H
#define GLOBALS_H

#include "mush.h"

typedef struct progstmt {
	STMT * stmt;
	struct progstmt* prev;
	struct progstmt* next;
} ProgStmt;

typedef struct kvpair {
	char* key;
	char* val;
	struct kvpair* next;
} KVPair;

#endif
