/*
 * This code was created by Peter Harvey (mostly during Christmas 98/99).
 * This code is LGPL. Please ensure that this message remains in future
 * distributions and uses of this code (thats about all I get out of it).
 * - Peter Harvey pharvey@codebydesign.com
 * 
 * This file has been modified for the MonetDB project.  See the file
 * Copyright in this directory for more information.
 */

/**********************************************
 * ODBCEnv.h
 *
 * Description:
 * This file contains the ODBC environment structure
 * and function prototypes on this structure.
 *
 * Author: Martin van Dinther
 * Date  : 30 aug 2002
 *
 **********************************************/

#ifndef _H_ODBCENV
#define _H_ODBCENV

#include "ODBCGlobal.h"
#include "ODBCError.h"


typedef struct tODBCDRIVERENV {
	/* Env properties */
	int Type;		/* structure type, used for handle validy test */
	ODBCError *Error;	/* pointer to an Error object or NULL */
	int RetrievedErrors;	/* # of errors already retrieved by SQLError */

	/* Env children: list of ODBC Connection handles created in this Env */
	void *FirstDbc;		/* first in list or NULL */
	enum {
		ODBC_2,
		ODBC_3,
	} ODBCVersion;
} ODBCEnv;



/*
 * Creates a new allocated ODBCEnv object and initializes it.
 *
 * Precondition: none
 * Postcondition: returns a new ODBCEnv object
 */
ODBCEnv *newODBCEnv(void);


/*
 * Check if the enviroment handle is valid.
 * Note: this function is used internally by the driver to assert legal
 * and save usage of the handle and prevent crashes as much as possible.
 *
 * Precondition: none
 * Postcondition: returns 1 if it is a valid environment handle,
 * 	returns 0 if is invalid and thus an unusable handle.
 */
int isValidEnv(ODBCEnv *env);


/*
 * Creates and adds an error msg object to the end of the error list of
 * this ODBCEnv struct.
 * When the errMsg is NULL and the SQLState is an ISO SQLState the
 * standard ISO message text for the SQLState is used as message.
 *
 * Precondition: env must be valid. SQLState and errMsg may be NULL.
 */
void addEnvError(ODBCEnv *env, const char *SQLState, const char *errMsg,
		 int nativeErrCode);


/*
 * Extracts an error object from the error list of this ODBCEnv struct.
 * The error object itself is removed from the error list.
 * The caller is now responsible for freeing the error object memory.
 *
 * Precondition: env and error must be valid
 * Postcondition: returns a ODBCError object or null when no error is available.
 */
ODBCError *getEnvError(ODBCEnv *env);


/* utility macro to quickly remove any none collected error msgs */
#define clearEnvErrors(env) do {					\
				assert(env);				\
				if (env->Error) {			\
					deleteODBCErrorList(env->Error); \
					env->Error = NULL;		\
					env->RetrievedErrors = 0;	\
				}					\
			} while (0)


/*
 * Destroys the ODBCEnv object including its own managed data.
 *
 * Precondition: env must be valid and no ODBCDbc objects may refer to this env.
 * Postcondition: env is completely destroyed, env handle is become invalid.
 */
void destroyODBCEnv(ODBCEnv *env);

#endif
