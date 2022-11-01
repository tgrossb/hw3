#include <stdlib.h>
#include <stdio.h>

#include "debug.h"
#include "globals.h"

/*
 * This is the "program store" module for Mush.
 * It maintains a set of numbered statements, along with a "program counter"
 * that indicates the current point of execution, which is either before all
 * statements, after all statements, or in between two statements.
 * There should be no fixed limit on the number of statements that the program
 * store can hold.
 */

ProgStmt* progStore = NULL;

// A negative progCounter indicates pointing to the end of the program
int progCounter = -1;

/**
 * @brief  Output a listing of the current contents of the program store.
 * @details  This function outputs a listing of the current contents of the
 * program store.  Statements are listed in increasing order of their line
 * number.  The current position of the program counter is indicated by
 * a line containing only the string "-->" at the current program counter
 * position.
 *
 * @param out  The stream to which to output the listing.
 * @return  0 if successful, -1 if any error occurred.
 */
int prog_list(FILE *out) {
	// Iterate over the progStore
	ProgStmt* current = progStore;
	while (current != NULL && current->stmt != NULL){
		if (current->stmt->lineno == progCounter)
			fprintf(out, "-->\n");
		show_stmt(out, current->stmt);
		current = current->next;
	}

	if (progCounter < 0)
		fprintf(out, "-->\n");

	return 0;
}

/**
 * @brief  Insert a new statement into the program store.
 * @details  This function inserts a new statement into the program store.
 * The statement must have a line number.  If the line number is the same as
 * that of an existing statement, that statement is replaced.
 * The program store assumes the responsibility for ultimately freeing any
 * statement that is inserted using this function.
 * Insertion of new statements preserves the value of the program counter:
 * if the position of the program counter was just before a particular statement
 * before insertion of a new statement, it will still be before that statement
 * after insertion, and if the position of the program counter was after all
 * statements before insertion of a new statement, then it will still be after
 * all statements after insertion.
 *
 * @param stmt  The statement to be inserted.
 * @return  0 if successful, -1 if any error occurred.
 */
int prog_insert(STMT *stmt) {
	if (stmt == NULL)
		return -1;

	// If the progstore is empty, insert the statement at the start and return
	if (progStore == NULL){
		progStore = malloc(sizeof(ProgStmt));
		progStore->stmt = stmt;
		progStore->prev = NULL;
		progStore->next = NULL;
		return 0;
	}

	// Otherwise find the location where the statement should be inserted
	ProgStmt* current = progStore;
	while (current != NULL && current->stmt != NULL){
		// If the line numbers match, replace the statement
		if (current->stmt->lineno == stmt->lineno){
			// Free the old statement
			free_stmt(current->stmt);
			// Insert the new statement
			current->stmt = stmt;
			// Return successful
			return 0;
		}

		// Otherwise, if the current statement is after the one to insert, put it before this one
		else if (current->stmt->lineno > stmt->lineno){
			// Create a new progstmt and link it to the others
			ProgStmt* new = malloc(sizeof(ProgStmt));
			new->stmt = stmt;
			new->prev = current->prev;
			new->next = current;

			// Link the prev to the new if there is a prev
			if (current->prev != NULL)
				current->prev->next = new;
			// If there isn't then this should be the beginning
			else
				progStore = new;

			// Link the next to the new
			current->prev = new;

			// Return successful
			return 0;
		}

		// If the next statement is null, we hit the end, insert the statement after this one
		else if (current->next == NULL){
			// Create a new progstmt and link it to the others
			ProgStmt* new = malloc(sizeof(ProgStmt));
			new->stmt = stmt;
			new->prev = current;
			new->next = NULL;

			// Link the prev to the new
			current->next = new;

			// Return successful
			return 0;
		}

		// If none of these match move on to the next statement
		current = current->next;
	}

	// If we've made it to here something wierd happened
	return -1;
}

/**
 * @brief  Delete statements from the program store.
 * @details  This function deletes from the program store statements whose
 * line numbers fall in a specified range.  Any deleted statements are freed.
 * Deletion of statements preserves the value of the program counter:
 * if before deletion the program counter pointed to a position just before
 * a statement that was not among those to be deleted, then after deletion the
 * program counter will still point the position just before that same statement.
 * If before deletion the program counter pointed to a position just before
 * a statement that was among those to be deleted, then after deletion the
 * program counter will point to the first statement beyond those deleted,
 * if such a statement exists, otherwise the program counter will point to
 * the end of the program.
 *
 * @param min  Lower end of the range of line numbers to be deleted.
 * @param max  Upper end of the range of line numbers to be deleted.
 */
int prog_delete(int min, int max) {
	// If the min and max are wierd return an error
	if (max < min)
		return -1;

	// Iterate through the program store deleting all statements in range
	ProgStmt* current = progStore;
	// Trying to delete from an empty program store is an error
	if (current->stmt == NULL)
		return -1;

	while (current != NULL && current->stmt != NULL){
		// If the current statement is within the (inclusive) delete range, remove it
		if (current->stmt->lineno >= min && current->stmt->lineno <= max){
			// Relink current->prev to current->next, skipping current
			if (current->prev != NULL)
				current->prev->next = current->next;
			// If the prev is null this is first, relink progStore to next
			else
				progStore = current->next;
			if (current->next != NULL)
				current->next->prev = current->prev;
			// Free current
			ProgStmt* tmp = current->next;
			free_stmt(current->stmt);
			current->stmt = NULL;
			free(current);
			current = tmp;
		}
		// Otherwise move on to the next statement (previous if handles moving on)
		else
			current = current->next;
	}

	// Now handle the program counter
	// If the program counter pointed to something before min or after max no changes need to be made
	if (progCounter < min || progCounter > max)
		return 0;

	// Otherwise, point it to first statement greater than progCounter
	// Do so by iterating again
	current = progStore;
	if (current == NULL){
		progCounter = -1;
		return 0;
	}
	while (current != NULL && current->stmt != NULL){
		// If we found a statement after the old progCounter, set progCounter to it and return
		if (current->stmt->lineno >= progCounter){
			progCounter = current->stmt->lineno;
			return 0;
		}

		// Otherwise if we are at the end of the store, set progCounter to the end of the program (-1)
		else if (current->next == NULL){
			progCounter = -1;
			return 0;
		}

		// Still otherwise just move to the next statement
		current = current->next;
	}

	// If we made it past the while something wierd happened
	return -1;
}

/**
 * @brief  Reset the program counter to the beginning of the program.
 * @details  This function resets the program counter to point just
 * before the first statement in the program.
 */
void prog_reset(void) {
	// Set progCounter to the lineno of the first statement
	// If there is no first statement set it to the end of the program (-1)
	if (progStore != NULL)
		progCounter = progStore->stmt->lineno;
	else
		progCounter = -1;
}

/**
 * @brief  Fetch the next program statement.
 * @details  This function fetches and returns the first program
 * statement after the current program counter position.  The program
 * counter position is not modified.  The returned pointer should not
 * be used after any subsequent call to prog_delete that deletes the
 * statement from the program store.
 *
 * @return  The first program statement after the current program
 * counter position, if any, otherwise NULL.
 */
STMT *prog_fetch(void) {
	// If the progCounter points to the end of the program return NULL
	if (progCounter < 0)
		return NULL;

	// Otherwise iterate over the progStore until we hit the stmt that matches progCounter
	ProgStmt* current = progStore;
	while (current != NULL && current->stmt != NULL){
		// If we have a match return the statement
		if (current->stmt->lineno == progCounter)
			return current->stmt;
		// Otherwise move to the next
		current = current->next;
	}

	// If we didn't find a match return null
	return NULL;
}

/**
 * @brief  Advance the program counter to the next existing statement.
 * @details  This function advances the program counter by one statement
 * from its original position and returns the statement just after the
 * new position.  The returned pointer should not be used after any
 * subsequent call to prog_delete that deletes the statement from the
 * program store.
 *
 * @return The first program statement after the new program counter
 * position, if any, otherwise NULL.
 */
STMT *prog_next() {
	// If the progCounter points to the end of the program return null
	if (progCounter < 0)
		return NULL;
	// Otherwise iterate over the progStore until we hit the stmt that matches progCounter
	ProgStmt* current = progStore;
	while (current != NULL && current->stmt != NULL){
		// If we have a match increase the program counter and return the next statement
		if (current->stmt->lineno == progCounter){
			// If this is the last statement point the program counter to the end of the program and return null
			if (current->next == NULL){
				progCounter = -1;
				return NULL;
			}
			// Otherwise point progCounter to the next statement and return that statement
			else {
				progCounter = current->next->stmt->lineno;
				return current->next->stmt;
			}
		}

		// Otherwise move on to the next statement
		current = current->next;
	}

	// If we made it past the while something weird happened
	return NULL;
}

/**
 * @brief  Perform a "go to" operation on the program store.
 * @details  This function performs a "go to" operation on the program
 * store, by resetting the program counter to point to the position just
 * before the statement with the specified line number.
 * The statement pointed at by the new program counter is returned.
 * If there is no statement with the specified line number, then no
 * change is made to the program counter and NULL is returned.
 * Any returned statement should only be regarded as valid as long
 * as no calls to prog_delete are made that delete that statement from
 * the program store.
 *
 * @return  The statement having the specified line number, if such a
 * statement exists, otherwise NULL.
 */
STMT *prog_goto(int lineno) {
	// Iterate over the progStore until we find a statement with a matching lineno
	ProgStmt* current = progStore;
	while (current != NULL && current->stmt != NULL){
		// If we have a lineno match set progCounter and return the current statement
		if (current->stmt->lineno == lineno){
			progCounter = lineno;
			return current->stmt;
		}

		// Otherwise go to the next statement
		current = current->next;
	}

	// If we made it past the while there is no match, return null
	return NULL;
}
