#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "globals.h"

/*
 * This is the "data store" module for Mush.
 * It maintains a mapping from variable names to values.
 * The values of variables are stored as strings.
 * However, the module provides functions for setting and retrieving
 * the value of a variable as an integer.  Setting a variable to
 * an integer value causes the value of the variable to be set to
 * a string representation of that integer.  Retrieving the value of
 * a variable as an integer is possible if the current value of the
 * variable is the string representation of an integer.
 */

KVPair* lookup = NULL;

/**
 * Compares two strings for equality.
 * @param str1 The first string to compare
 * @param str2 The second string to compare
 * @return -1 if the strings are not equal, 1 if they are equal
 */
int kvStrEq(char* str1, char* str2){
	for (int c=0; str1[c] != '\0' || str2[c] != '\0'; c++)
		if (str1[c] != str2[c])
			return -1;
	return 1;
}

/**
 * @brief  Get the current value of a variable as a string.
 * @details  This function retrieves the current value of a variable
 * as a string.  If the variable has no value, then NULL is returned.
 * Any string returned remains "owned" by the data store module;
 * the caller should not attempt to free the string or to use it
 * after any subsequent call that would modify the value of the variable
 * whose value was retrieved.  If the caller needs to use the string for
 * an indefinite period, a copy should be made immediately.
 *
 * @param  var  The variable whose value is to be retrieved.
 * @return  A string that is the current value of the variable, if any,
 * otherwise NULL.
 */
char *store_get_string(char *var) {
	// Iterate over the lookup table looking for a matching key
	KVPair* current = lookup;
	while (current != NULL){
		if (kvStrEq(current->key, var) > 0)
			return current->val;
		current = current->next;
	}
	return NULL;
}

/**
 * @brief  Get the current value of a variable as an integer.
 * @details  This retrieves the current value of a variable and
 * attempts to interpret it as an integer.  If this is possible,
 * then the integer value is stored at the pointer provided by
 * the caller.
 *
 * @param  var  The variable whose value is to be retrieved.
 * @param  valp  Pointer at which the returned value is to be stored.
 * @return  If the specified variable has no value or the value
 * cannot be interpreted as an integer, then -1 is returned,
 * otherwise 0 is returned.
 */
int store_get_int(char *var, long *valp) {
	// Get the string version of the variable first
	char* val = store_get_string(var);
	if (val == NULL)
		return -1;

	// Interpret the string as an integer
	// Keep track of pos/neg in mult, the building value in build, and string position in c
	int mult = 1;
	int build = 0;
	int c=0;
	if (val[0] == '-'){
		mult = -1;
		c = 1;
	}
	for (; val[c] != '\0'; c++){
		// If it is not a numeric character return -1
		if (val[c] < '0' || val[c] > '9')
			return -1;
		// Otherwise add it as a digit to build
		build = build*10 + (val[c] - '0');
	}
	*valp = build * mult;
	return 0;
}

/**
 * @brief  Set the value of a variable as a string.
 * @details  This function sets the current value of a specified
 * variable to be a specified string.  If the variable already
 * has a value, then that value is replaced.  If the specified
 * value is NULL, then any existing value of the variable is removed
 * and the variable becomes un-set.  Ownership of the variable and
 * the value strings is not transferred to the data store module as
 * a result of this call; the data store module makes such copies of
 * these strings as it may require.
 *
 * @param  var  The variable whose value is to be set.
 * @param  val  The value to set, or NULL if the variable is to become
 * un-set.
 */
int store_set_string(char *var, char *val) {
	// Iterate over the lookup table looking for a matching key
	KVPair* current = lookup;
	KVPair* last = NULL;
	while (current != NULL){
		// If we have a match, save the val into the kvpair and return storing's success
		if (kvStrEq(current->key, var) > 0){
			if (current->val != NULL)
				free(current->val);
			current->val = strdup(val);
			return 0;
		}
		// If the next kvpair is null we hit the end without finding a match
		// Store this as the last element and allow the loop to end
		if (current->next == NULL)
			last = current;

		// Move on to the next kvpair
		current = current->next;
	}

	// If we made it to here we need to create a new kvpair
	KVPair* new = malloc(sizeof(KVPair));
	new->key = strdup(var);
	new->val = strdup(val);
	new->next = NULL;

	// If the lookup is null insert new as the first kvpair
	if (lookup == NULL){
		lookup = new;
		return 0;
	}
	// Otherwise, last should be set, insert as the successor to last
	else if (last != NULL){
		last->next = new;
		return 0;
	}
	// Other otherwise, something went wrong
	return -1;
}

/**
 * @brief  Set the value of a variable as an integer.
 * @details  This function sets the current value of a specified
 * variable to be a specified integer.  If the variable already
 * has a value, then that value is replaced.  Ownership of the variable
 * string is not transferred to the data store module as a result of
 * this call; the data store module makes such copies of this string
 * as it may require.
 *
 * @param  var  The variable whose value is to be set.
 * @param  val  The value to set.
 */
int store_set_int(char *var, long val) {
	// Convert the val long into a string
	int length = snprintf(NULL, 0, "%ld", val);
	char str[length+1];
	snprintf(str, length+1, "%ld", val);

	// Use the store_set_string method to do the rest of the work noew
	return store_set_string(var, str);
}

/**
 * @brief  Print the current contents of the data store.
 * @details  This function prints the current contents of the data store
 * to the specified output stream.  The format is not specified; this
 * function is intended to be used for debugging purposes.
 *
 * @param f  The stream to which the store contents are to be printed.
 */
void store_show(FILE *f) {
	fprintf(f, "Data store:\n");
	// Iterate over the lookup table
	KVPair* current = lookup;
	while (current != NULL){
		fprintf(f, "\t%s:\t\"%s\"\n", current->key, current->val);
		current = current->next;
	}
}
