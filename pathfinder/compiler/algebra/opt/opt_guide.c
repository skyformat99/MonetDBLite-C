/**
 * @file
 *
 * Optimize relational algebra expression DAG 
 *  based on guide nodes.
 * (This requires no burg pattern matching as we
 *  apply optimizations in a peep-hole style on 
 *  single nodes only.) 
 *
 * Copyright Notice:
 * -----------------
 *
 * The contents of this file are subject to the Pathfinder Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 * http://monetdb.cwi.nl/Legal/PathfinderLicense-1.1.html
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied.  See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the Pathfinder system.
 *
 * The Original Code has initially been developed by the Database &
 * Information Systems Group at the University of Konstanz, Germany and
 * is now maintained by the Database Systems Group at the Technische
 * Universitaet Muenchen, Germany.  Portions created by the University of
 * Konstanz and the Technische Universitaet Muenchen are Copyright (C)
 * 2000-2005 University of Konstanz and (C) 2005-2007 Technische
 * Universitaet Muenchen, respectively.  All Rights Reserved.
 *
 * $Id$
 */

#include "pathfinder.h"
#include <assert.h>

#include "algopt.h"
#include "alg_dag.h"


static void opt_guide (PFla_op_t *n);

/* worker for PFalgopt_guide */
static void
opt_guide (PFla_op_t *n)
{
    (void) n;
    return;
}

/**
  * Invoke algebra optimization.
 */
PFla_op_t* 
PFalgopt_guide(PFla_op_t *root, PFguide_tree_t *guide)
{

    PFprop_infer_guide(root, guide);

    /* Optimize algebra tree */
    opt_guide(root);
    PFla_dag_reset(root);

    return root;
}
