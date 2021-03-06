/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/rule_engine.h>
#include "rule_engine_private.h"

#include "engine_private.h"
#include "rule_logger_private.h"

#include <ironbee/action.h>
#include <ironbee/bytestr.h>
#include <ironbee/capture.h>
#include <ironbee/config.h>
#include <ironbee/context.h>
#include <ironbee/core.h>
#include <ironbee/engine.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/flags.h>
#include <ironbee/mm.h>
#include <ironbee/operator.h>
#include <ironbee/rule_logger.h>
#include <ironbee/string.h>
#include <ironbee/transformation.h>
#include <ironbee/util.h>

#include <assert.h>
#include <inttypes.h>

/**
 * Phase Flags
 */
#define PHASE_FLAG_NONE          (0x0)     /**< No phase flags */
#define PHASE_FLAG_IS_VALID      (1 <<  0) /**< Phase is valid */
#define PHASE_FLAG_IS_STREAM     (1 <<  1) /**< Phase is steam inspection */
#define PHASE_FLAG_ALLOW_CHAIN   (1 <<  2) /**< Rule allows chaining */
#define PHASE_FLAG_ALLOW_TFNS    (1 <<  3) /**< Rule allows transformations */
#define PHASE_FLAG_FORCE         (1 <<  4) /**< Force execution for phase */
#define PHASE_FLAG_REQUEST       (1 <<  5) /**< One of the request phases */
#define PHASE_FLAG_RESPONSE      (1 <<  6) /**< One of the response phases */
#define PHASE_FLAG_POSTPROCESS   (1 <<  7) /**< Post process phase */
#define PHASE_FLAG_LOGGING       (1 <<  8) /**< Logging phase */

static const char *indexed_keys[] = {
    "FIELD",
    "FIELD_TFN",
    "FIELD_TARGET",
    "FIELD_NAME",
    "FIELD_NAME_FULL",
    NULL
};

/**
 * Data on each rule phase, one per phase.
 */
struct ib_rule_phase_meta_t {
    bool                   is_stream;
    ib_rule_phase_num_t    phase_num;
    ib_state_hook_type_t   hook_type;
    ib_flags_t             flags;
    const char            *name;
    const char            *description;
    ib_flags_t             required_op_flags;
    ib_state_t             state;
};

/* Rule definition data */
static const ib_rule_phase_meta_t rule_phase_meta[] =
{
    {
        false,
        IB_PHASE_NONE,
        (ib_state_hook_type_t) -1,
        ( PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS ),
        NULL,
        "Generic 'Phase' Rule",
        IB_OP_CAPABILITY_NONE,
        (ib_state_t) -1
    },
    {
        false,
        IB_PHASE_REQUEST_HEADER,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_REQUEST ),
        "REQUEST_HEADER",
        "Request Header",
        IB_OP_CAPABILITY_NONE,
        handle_request_header_state
    },
    {
        false,
        IB_PHASE_REQUEST_HEADER_PROCESS,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_REQUEST ),
        "REQUEST_HEADER_PROCESS",
        "Request Header Process",
        IB_OP_CAPABILITY_NONE,
        handle_request_header_state
    },
    {
        false,
        IB_PHASE_REQUEST,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_REQUEST ),
        "REQUEST",
        "Request",
        IB_OP_CAPABILITY_NONE,
        handle_request_state
    },
    {
        false,
        IB_PHASE_REQUEST_PROCESS,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_REQUEST ),
        "REQUEST_PROCESS",
        "Request Process",
        IB_OP_CAPABILITY_NONE,
        handle_request_state
    },
    {
        false,
        IB_PHASE_RESPONSE_HEADER,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE_HEADER",
        "Response Header",
        IB_OP_CAPABILITY_NONE,
        handle_response_header_state
    },
    {
        false,
        IB_PHASE_RESPONSE_HEADER_PROCESS,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE_HEADER_PROCESS",
        "Response Header Process",
        IB_OP_CAPABILITY_NONE,
        handle_response_header_state
    },
    {
        false,
        IB_PHASE_RESPONSE,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE",
        "Response",
        IB_OP_CAPABILITY_NONE,
        handle_response_state
    },
    {
        false,
        IB_PHASE_RESPONSE_PROCESS,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE_PROCESS",
        "Response Process",
        IB_OP_CAPABILITY_NONE,
        handle_response_state
    },
    {
        false,
        IB_PHASE_POSTPROCESS,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_FORCE |
          PHASE_FLAG_POSTPROCESS ),
        "POSTPROCESS",
        "Post Process",
        IB_OP_CAPABILITY_NONE,
        handle_postprocess_state
    },
    {
        false,
        IB_PHASE_LOGGING,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_ALLOW_CHAIN |
          PHASE_FLAG_ALLOW_TFNS |
          PHASE_FLAG_FORCE |
          PHASE_FLAG_LOGGING ),
        "LOGGING",
        "Logging",
        IB_OP_CAPABILITY_NONE,
        handle_logging_state
    },

    /* Stream rule phases */
    {
        true,
        IB_PHASE_NONE,
        (ib_state_hook_type_t) -1,
        (PHASE_FLAG_IS_STREAM),
        NULL,
        "Generic 'Stream Inspection' Rule",
        IB_OP_CAPABILITY_NONE,
        (ib_state_t) -1
    },
    {
        true,
        IB_PHASE_REQUEST_HEADER_STREAM,
        IB_STATE_HOOK_TX,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_IS_STREAM |
          PHASE_FLAG_REQUEST ),
        "REQUEST_HEADER_STREAM",
        "Request Header Stream",
        IB_OP_CAPABILITY_NONE,
        handle_context_tx_state
    },
    {
        true,
        IB_PHASE_REQUEST_BODY_STREAM,
        IB_STATE_HOOK_TXDATA,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_IS_STREAM |
          PHASE_FLAG_REQUEST ),
        "REQUEST_BODY_STREAM",
        "Request Body Stream",
        IB_OP_CAPABILITY_NONE,
        request_body_data_state
    },
    {
        true,
        IB_PHASE_RESPONSE_HEADER_STREAM,
        IB_STATE_HOOK_HEADER,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_IS_STREAM |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE_HEADER_STREAM",
        "Response Header Stream",
        IB_OP_CAPABILITY_NONE,
        response_header_data_state
    },
    {
        true,
        IB_PHASE_RESPONSE_BODY_STREAM,
        IB_STATE_HOOK_TXDATA,
        ( PHASE_FLAG_IS_VALID |
          PHASE_FLAG_IS_STREAM |
          PHASE_FLAG_RESPONSE ),
        "RESPONSE_BODY_STREAM",
        "Response Body Stream",
        IB_OP_CAPABILITY_NONE,
        response_body_data_state
    },
    {
        false,
        IB_PHASE_INVALID,
        (ib_state_hook_type_t) -1,
        PHASE_FLAG_NONE,
        NULL,
        "Invalid",
        IB_OP_CAPABILITY_NONE,
        (ib_state_t) -1
    }
};


/**
 * Data on enable directives.
 */
typedef struct {
    const char            *name;           /**< Name used in logging. */
    const char            *file;           /**< Configuration file of enable */
    unsigned int           lineno;         /**< Line number in config file */
    bool                   require_match;  /**< Is at least 1 match required? */
    /* Enable (or disable) the rule. on match. */
    bool                   enable;
    //! A function that determines if a particular rule should be enabled.
    ib_rule_enable_fn_t    rule_enable_fn;
    //! Callback data for rule_enable_fn.
    void                  *rule_enable_cbdata;
} ib_rule_enable_t;

/**
 * Items on the rule execution object stack
 */
typedef struct {
    ib_rule_t              *rule;        /**< The currently rule */
    ib_rule_log_exec_t     *exec_log;    /**< Rule execution logging object */
    ib_rule_target_t       *target;      /**< The current rule target */
    ib_num_t                result;      /**< Rule execution result */
} rule_exec_stack_frame_t;

/**
 * The rule engine uses recursion to walk through lists and chains.  These
 * define the limits of the recursion depth.
 */
#define MAX_LIST_RECURSION   (5)       /**< Max list recursion limit */
#define MAX_CHAIN_RECURSION  (10)      /**< Max chain recursion limit */

ib_status_t ib_rule_set_invert(ib_rule_t *rule, bool invert)
{
    assert(rule != NULL);
    assert(rule->opinst != NULL);

    rule->opinst->invert = invert;

    return IB_OK;
}

ib_status_t ib_rule_set_op_params(ib_rule_t *rule, const char *params)
{
    assert(rule != NULL);
    assert(rule->ctx != NULL);
    assert(rule->opinst != NULL);
    assert(params != NULL);

    ib_status_t rc;

    rule->opinst->params = ib_mm_strdup(rule->ctx->mm, params);
    if (rule->opinst->params == NULL) {
        return IB_EALLOC;
    }

    rc = ib_field_create_bytestr_alias(
        &(rule->opinst->fparam),
        rule->ctx->mm,
        "",
        0,
        (uint8_t *)rule->opinst->params,
        strlen(rule->opinst->params));
    if (rc != IB_OK) {
        return rc;
    }


    return IB_OK;
}

ib_rule_phase_num_t ib_rule_lookup_phase(
    const char *name,
    bool        is_stream)
{
    const ib_rule_phase_meta_t *item;

    for (item = rule_phase_meta; item->phase_num != IB_PHASE_INVALID; ++item) {
        if ( (item->name != NULL) && (strcasecmp(name, item->name) == 0) ) {
             if (item->is_stream != is_stream) {
                 return IB_PHASE_INVALID;
             }
             return item->phase_num;
         }
    }
    return IB_PHASE_INVALID;
}

/**
 * Test the validity of a phase number
 *
 * @param phase_num Phase number to check
 *
 * @returns true if @a phase_num is valid, false if not.
 */
static inline bool is_phase_num_valid(ib_rule_phase_num_t phase_num)
{
    return (phase_num >= IB_PHASE_NONE) && (phase_num < IB_RULE_PHASE_COUNT);
}

/**
 * Find a rule's matching phase meta data, matching the only the phase number.
 *
 * @param[in] phase_num Phase number (IB_PHASE_xxx)
 * @param[out] phase_meta Matching rule phase meta-data
 *
 * @returns Status code
 */
static ib_status_t find_phase_meta(ib_rule_phase_num_t phase_num,
                                   const ib_rule_phase_meta_t **phase_meta)
{
    const ib_rule_phase_meta_t *meta;

    assert (phase_meta != NULL);
    assert (is_phase_num_valid(phase_num));

    /* Loop through all parent rules */
    for (meta = rule_phase_meta;  meta->phase_num != IB_PHASE_INVALID;  ++meta)
    {
        if (meta->phase_num == phase_num) {
            *phase_meta = meta;
            return IB_OK;
        }
    }
    return IB_ENOENT;
}

/**
 * Return a phase's name string
 *
 * @param[in] phase_meta Rule phase meta-data
 *
 * @returns Name string
 */
static const char *phase_name(
    const ib_rule_phase_meta_t *phase_meta)
{
    assert (phase_meta != NULL);
    if (phase_meta->name != NULL) {
        return phase_meta->name;
    }
    else {
        return phase_meta->description;
    }
}

/**
 * Return a phase's description string
 *
 * @param[in] phase_meta Rule phase meta-data
 *
 * @returns Description string
 */
static const char *phase_description(
    const ib_rule_phase_meta_t *phase_meta)
{
    assert (phase_meta != NULL);
    return phase_meta->description;
}

/**
 * Find a rule's matching phase meta data, matching the phase number and
 * the phase's stream type.
 *
 * @param[in] is_stream true if this is a "stream inspection" rule
 * @param[in] phase_num Phase number (IB_PHASE_xxx)
 * @param[out] phase_meta Matching rule phase meta-data
 *
 * @note If @a is_stream is IB_TRI_UNSET, this function will return the
 * first phase meta-data object that matches the phase number.
 *
 * @returns Status code
 */
static ib_status_t find_meta(
    bool is_stream,
    ib_rule_phase_num_t phase_num,
    const ib_rule_phase_meta_t **phase_meta)
{
    const ib_rule_phase_meta_t *meta;

    assert (phase_meta != NULL);
    assert (is_phase_num_valid(phase_num));

    /* Loop through all parent rules */
    for (meta = rule_phase_meta;  meta->phase_num != IB_PHASE_INVALID;  ++meta)
    {
        if ( (meta->phase_num == phase_num) &&
             (is_stream == meta->is_stream) )
        {
            *phase_meta = meta;
            return IB_OK;
        }
    }
    return IB_ENOENT;
}

/**
 * Get a capture collection for a rule_exec.
 *
 * @param[in] rule_exec Rule execution environment.
 * @return Capture collection or NULL if unneeded or error (will log).
 **/
static ib_field_t *get_capture(ib_rule_exec_t *rule_exec)
{
    ib_field_t *capture = NULL;
    ib_status_t rc;
    if (
        rule_exec->rule != NULL &&
        ib_flags_all(rule_exec->rule->flags, IB_RULE_FLAG_CAPTURE)
    )
    {
        rc = ib_capture_acquire(
            rule_exec->tx,
            rule_exec->rule->capture_collection,
            &capture
        );
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                "Failed to create capture collection: %s",
                ib_status_to_string(rc)
            );
            capture = NULL;
        }
    }
    return capture;
}

/**
 * Create a rule execution object
 *
 * @param[in] tx Transaction.
 * @param[out] rule_exec Rule execution object (or NULL)
 *
 * @returns
 *   - IB_OK on success.
 */
static ib_status_t rule_exec_create(ib_tx_t *tx,
                                    ib_rule_exec_t **rule_exec)
{
    assert(tx != NULL);

    ib_status_t rc;
    ib_rule_exec_t *exec;

    /* Create the execution object */
    exec = (ib_rule_exec_t *)ib_mm_alloc(tx->mm, sizeof(*exec));
    if (exec == NULL) {
        return IB_EALLOC;
    }
    exec->ib = tx->ib;
    exec->tx = tx;

    /* Create the rule stack */
    rc = ib_list_create(&(exec->rule_stack), tx->mm);
    if (rc != IB_OK) {
        ib_rule_log_tx_error(tx, "Failed to create rule stack: %s",
                             ib_status_to_string(rc));
        return rc;
    }

    /* Create the phase rule list */
    rc = ib_list_create(&(exec->phase_rules), tx->mm);
    if (rc != IB_OK) {
        ib_rule_log_tx_error(tx, "Failed to create phase rule list: %s",
                             ib_status_to_string(rc));
        return rc;
    }

    /* Create the value stack */
    rc = ib_list_create(&(exec->value_stack), tx->mm);
    if (rc != IB_OK) {
        ib_rule_log_tx_error(tx, "Failed to create value stack: %s",
                             ib_status_to_string(rc));
        return rc;
    }

    /* Create the TX log object */
    rc = ib_rule_log_tx_create(exec, &(exec->tx_log));
    if (rc != IB_OK) {
        ib_rule_log_tx_warn(tx, "Failed to create rule execution logger: %s",
                            ib_status_to_string(rc));
    }

    /* No current rule, target, etc. */
    exec->rule = NULL;
    exec->target = NULL;
    exec->cur_status = IB_OK;
    exec->cur_result = 0;
    exec->rule_status = IB_OK;
    exec->rule_result = 0;
    exec->exec_log = NULL;

#ifdef IB_RULE_TRACE
    exec->traces = ib_mm_calloc(
        tx->mm,
        exec->ib->rule_engine->index_limit,
        sizeof(*(exec->traces))
    );
#endif

    /* Pass the new object back to the caller if required */
    if (rule_exec != NULL) {
        *rule_exec = exec;
    }
    return IB_OK;
}

/**
 * Push a rule onto the rule execution object's rule stack
 *
 * @param[in,out] rule_exec Rule execution object
 * @param[in] rule The executing rule (or NULL to reset)
 *
 * @returns
 *   - IB_OK on success.
 */
static ib_status_t rule_exec_push_rule(ib_rule_exec_t *rule_exec,
                                       const ib_rule_t *rule)

{
    assert(rule_exec != NULL);
    assert(rule != NULL);

    ib_status_t              rc;
    ib_rule_log_exec_t      *exec_log;
    rule_exec_stack_frame_t *frame;

    rule_exec->rule = NULL;
    rule_exec->target = NULL;
    rule_exec->rule_status = IB_OK;
    rule_exec->rule_result = 0;

    /* Create a new stack frame */
    frame = ib_mm_alloc(rule_exec->tx->mm, sizeof(*frame));
    if (frame == NULL) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Failed to allocate stack frame");
        return IB_EALLOC;
    }

    /* Fill in the stack frame from the current state and push it */
    frame->rule = rule_exec->rule;
    frame->exec_log = rule_exec->exec_log;
    frame->target = rule_exec->target;
    frame->result = rule_exec->rule_result;
    rc = ib_list_push(rule_exec->rule_stack, frame);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Failed to add rule to rule stack: %s",
                          ib_status_to_string(rc));
        return rc;
    }

    /* Add the rule to the object *before* creating the rule exec logger */
    rule_exec->rule = (ib_rule_t *)rule;

    /* Create a new execution logging object */
    rc = ib_rule_log_exec_create(rule_exec, &exec_log);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Failed to create log object: %s",
                          ib_status_to_string(rc));
    }
    rule_exec->exec_log = exec_log;

    return IB_OK;
}

/**
 * Pop the the top rule of the rule stack
 *
 * @param[in,out] rule_exec Rule execution object
 *
 * @returns
 *   - IB_OK on success.
 */
static ib_status_t rule_exec_pop_rule(ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);

    ib_status_t              rc;
    rule_exec_stack_frame_t *frame;

    rc = ib_list_pop(rule_exec->rule_stack, &frame);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Failed to pop rule from stack: %s",
                          ib_status_to_string(rc));
        return rc;
    }

    /* Copy the items from the stack frame into the rule execution object */
    rule_exec->rule = frame->rule;
    rule_exec->rule = frame->rule;
    rule_exec->target = frame->target;
    rule_exec->rule_result = frame->result;

    return IB_OK;
}

/**
 * Push a value onto a rule execution object's value stack
 *
 * @param[in,out] rule_exec The rule execution object
 * @param[in] value The value to push
 *
 * @returns true if the value was successfully pushed
 */
static bool rule_exec_push_value(ib_rule_exec_t *rule_exec,
                                 const ib_field_t *value)
{
    assert(rule_exec != NULL);
    ib_status_t rc;

    rc = ib_list_push(rule_exec->value_stack, (ib_field_t *)value);
    if (rc != IB_OK) {
        ib_rule_log_warn(rule_exec,
                         "Failed to push value onto value stack: %s",
                         ib_status_to_string(rc));
        return false;
    }
    return true;
}

/**
 * Pop the value off the rule execution object's value stack
 *
 * @param[in,out] rule_exec The rule execution object
 * @param[in] pushed true if the value was successfully pushed
 *
 */
static void rule_exec_pop_value(ib_rule_exec_t *rule_exec,
                                bool pushed)
{
    assert(rule_exec != NULL);
    ib_status_t rc;

    if (! pushed) {
        return;
    }
    rc = ib_list_pop(rule_exec->value_stack, NULL);
    if (rc != IB_OK) {
        ib_rule_log_warn(rule_exec,
                         "Failed to pop value from value stack: %s",
                         ib_status_to_string(rc));
    }
    return;
}

/**
 * Execute a single transformation on a target.
 *
 * @param[in] rule_exec The rule execution object.
 * @param[in] tfn_inst The transformation instance to execute.
 * @param[in] value Initial value of the target field.
 * @param[out] result Pointer to field in which to store the result.
 *
 * @returns Status code
 */
static ib_status_t execute_tfn_single(const ib_rule_exec_t  *rule_exec,
                                      const ib_transformation_inst_t   *tfn_inst,
                                      const ib_field_t      *value,
                                      const ib_field_t     **result)
{
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);

    ib_status_t       rc;
    const ib_field_t *out = NULL;

    rc = ib_transformation_inst_execute(tfn_inst, rule_exec->tx->mm, value, &out);
    ib_rule_log_exec_tfn_value(rule_exec->exec_log, value, out, rc);

    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Error transforming \"%.*s\" "
                          "for transformation \"%s\": %s",
                          (int)value->nlen, value->name,
                          ib_transformation_name(
                              ib_transformation_inst_transformation(tfn_inst)
                          ),
                          ib_status_to_string(rc));
        return rc;
    }
    if (out == NULL) {
        ib_rule_log_error(rule_exec,
                          "Error transforming \"%.*s\" "
                          "for transformation \"%s\": "
                          "Transformation returned NULL",
                          (int)value->nlen, value->name,
                          ib_transformation_name(
                              ib_transformation_inst_transformation(tfn_inst)
                          ));
        return IB_EINVAL;
    }

    assert(rc == IB_OK);
    *result = out;

    return IB_OK;
}

/**
 * Execute list of transformations on a target.
 *
 * @param[in] rule_exec The rule execution object
 * @param[in] value Initial value of the target field
 * @param[out] result Pointer to field in which to store the result
 *
 * @returns Status code
 */
static ib_status_t execute_tfns(const ib_rule_exec_t *rule_exec,
                                const ib_field_t *value,
                                const ib_field_t **result)
{
    assert(rule_exec != NULL);
    assert(result != NULL);

    ib_status_t           rc;
    const ib_list_node_t *node = NULL;
    const ib_field_t     *in_field;
    const ib_field_t     *out = NULL;

    /* No transformations?  Do nothing. */
    if (value == NULL) {
        *result = NULL;
        return IB_OK;
    }
    else if (ib_list_elements(rule_exec->target->tfn_list) == 0) {
        *result = value;
        ib_rule_log_trace(rule_exec, "No transformations");
        return IB_OK;
    }

    ib_rule_log_trace(rule_exec, "Executing %zd transformations",
                      ib_list_elements(rule_exec->target->tfn_list));

    /*
     * Loop through all of the target's transformations.
     */
    in_field = value;
    IB_LIST_LOOP_CONST(rule_exec->target->tfn_list, node) {
        const ib_transformation_inst_t  *tfn_inst =
            (const ib_transformation_inst_t *)ib_list_node_data_const(node);

        /* Run it */
        ib_rule_log_trace(
            rule_exec,
            "Executing transformation %s",
            ib_transformation_name(
                ib_transformation_inst_transformation(tfn_inst)
            ));
        ib_rule_log_exec_tfn_inst_add(rule_exec->exec_log, tfn_inst);
        rc = execute_tfn_single(rule_exec, tfn_inst, in_field, &out);
        if (rc != IB_OK) {
            ib_rule_log_error(
                rule_exec,
                "Error executing target transformation %s: %s",
                ib_transformation_name(
                    ib_transformation_inst_transformation(tfn_inst)
                ),
                ib_status_to_string(rc)
            );
        }
        ib_rule_log_exec_tfn_inst_fin(
            rule_exec->exec_log,
            tfn_inst,
            in_field,
            out,
            rc);

        /* Verify that out isn't NULL */
        if (out == NULL) {
            ib_rule_log_error(
                rule_exec,
                "Target transformation %s returned NULL",
                ib_transformation_name(
                    ib_transformation_inst_transformation(tfn_inst)
                )
            );
            return IB_EINVAL;
        }

        /* The output of the operator is now input for the next field op. */
        in_field = out;
    }

    /* The output of the final operator is the result */
    *result = out;

    /* Done. */
    return IB_OK;
}

/**
 * Execute a single rule action
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] result Rule execution result
 * @param[in] action The action to execute
 *
 * @returns Status code
 */
static ib_status_t execute_action(const ib_rule_exec_t *rule_exec,
                                  ib_num_t result,
                                  const ib_action_inst_t *action)
{
    ib_status_t   rc;
    const char   *name = (result != 0) ? "True" : "False";

    ib_rule_log_trace(rule_exec,
                      "Executing %s rule action %s",
                      name, ib_action_name(ib_action_inst_action(action)));

    {
        ib_list_node_t *node;
        IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.pre_action, node) {
            const ib_rule_pre_action_hook_t *hook =
                (const ib_rule_pre_action_hook_t *)
                    ib_list_node_data_const(node);
            hook->fn(rule_exec, action, result, hook->data);
        }
    }

    /* Run it, check the results */
    rc = ib_action_inst_execute(action, rule_exec);
    if ( rc != IB_OK ) {
        ib_rule_log_error(rule_exec,
                          "Action \"%s\" returned an error: %s",
                          ib_action_name(ib_action_inst_action(action)),
                          ib_status_to_string(rc));
    }


    {
        ib_list_node_t *node;
        IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.post_action, node) {
            const ib_rule_post_action_hook_t *hook =
                (const ib_rule_post_action_hook_t *)
                    ib_list_node_data_const(node);
            hook->fn(rule_exec, action, result, rc, hook->data);
        }
    }

    return IB_OK;
}

/**
 * Execute a rule's actions
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] actions List of actions to execute
 * @param[in] name Name for logging
 * @param[in] log Log the actions?
 *
 * @returns Status code
 */
static ib_status_t execute_action_list(const ib_rule_exec_t *rule_exec,
                                       const ib_list_t *actions,
                                       const char *name,
                                       bool log)
{
    assert(rule_exec != NULL);

    const ib_list_node_t *node = NULL;
    ib_status_t           rc = IB_OK;

    if (actions == NULL) {
        return IB_OK;
    }

    if (log) {
        ib_rule_log_trace(rule_exec, "Executing rule %s actions", name);
    }

    /*
     * Loop through all of the fields
     *
     * @todo The current behavior is to keep running even after an action
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP_CONST(actions, node) {
        ib_status_t       arc;     /* Action's return code */
        const ib_action_inst_t *action =
            (const ib_action_inst_t *)ib_list_node_data_const(node);

        /* Execute the action */
        arc = execute_action(rule_exec, rule_exec->cur_result, action);
        if (log) {
            ib_rule_log_exec_add_action(rule_exec->exec_log, action, arc);
        }

        /* Record an error status code unless a block rc is to be reported. */
        if ( (arc != IB_OK) && log) {
            ib_rule_log_error(rule_exec,
                              "Action %s/\"%s\" returned an error: %s",
                              name,
                              ib_action_name(ib_action_inst_action(action)),
                              ib_status_to_string(arc));
            rc = arc;
        }
    }

    return rc;
}

/**
 * Store a rules results
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] value Current target value
 * @param[in] op_rc Operator result code
 * @param[in] result Rule execution result
 *
 * @returns Action status code
 */
static ib_status_t store_results(ib_rule_exec_t *rule_exec,
                                 const ib_field_t *value,
                                 ib_status_t op_rc,
                                 ib_num_t result)
{
    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(rule_exec->rule->opinst != NULL);

    /* Store the value */
    rule_exec->cur_value = value;

    /* Perform inversion if required */
    if (rule_exec->rule->opinst->invert) {
        result = (result == 0);
    }

    /* Store the result after inversion */
    rule_exec->cur_result = result;
    if (result != 0) {
        rule_exec->rule_result = result;
    }

    /* Store the operations result */
    rule_exec->cur_status = op_rc;
    if (op_rc != IB_OK) {
        rule_exec->rule_status = op_rc;
    }

    /* Return the primary actions' result code */
    return IB_OK;
}

/**
 * Execute a rule's actions
 *
 * @todo The current behavior is to keep running even after action(s)
 * returns an error.  This needs further discussion to determine what
 * the correct behavior should be.
 *
 * @param[in] rule_exec Rule execution object
 *
 * @returns Action status code
 */
static ib_status_t execute_rule_actions(const ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(rule_exec->rule->opinst != NULL);

    const ib_list_t    *main_actions = NULL;
    const char         *name = NULL;
    const ib_list_t    *aux_actions = NULL;
    ib_status_t         rc;

    /* Choose the appropriate action list */
    if (rule_exec->cur_status == IB_OK) {
        if (rule_exec->cur_result != 0) {
            main_actions = rule_exec->rule->true_actions;
            name = "True";
        }
        else {
            main_actions = rule_exec->rule->false_actions;
            name = "False";
        }
    }
    aux_actions = rule_exec->rule->aux_actions;

    /* Run the main actions */
    ib_rule_log_exec_add_result(rule_exec->exec_log,
                                rule_exec->cur_value,
                                rule_exec->cur_result);
    rc = execute_action_list(rule_exec, main_actions, name, true);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Error executing action(s) for rule: %s",
                          ib_status_to_string(rc));
    }

    /* Run any auxiliary actions, ignore result, don't log */
    execute_action_list(rule_exec, aux_actions, "Auxiliary",
                        ib_rule_log_level(rule_exec->tx->ctx) >= IB_LOG_DEBUG);

    /* Return the primary actions' result code */
    return rc;
}

/**
 * Clear the target fields (FIELD, FIELD_NAME, FIELD_NAME_FULL)
 *
 * @param[in] rule_exec Rule execution object
 *
 */
static void clear_target_fields(ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);
    assert(rule_exec->tx->var_store != NULL);

    const ib_rule_engine_t *re = rule_exec->ib->rule_engine;
    ib_var_store_t *var_store = rule_exec->tx->var_store;

    /* Destroy FIELD targets */
    if (! ib_flags_any(rule_exec->rule->flags, IB_RULE_FLAG_FIELDS)) {
        return;
    }

    ib_rule_log_trace(rule_exec, "Destroying target fields");

    ib_var_source_set(re->source.field, var_store, NULL);
    ib_var_source_set(re->source.field_target, var_store, NULL);
    ib_var_source_set(re->source.field_tfn, var_store, NULL);
    ib_var_source_set(re->source.field_name, var_store, NULL);
    ib_var_source_set(re->source.field_name_full, var_store, NULL);

    return;
}

/**
 * Fetch an existing field, by name, or add a new @ref IB_FTYPE_GENERIC field.
 *
 * The new field will point to NULL and should be updated to
 * a new type and value by the caller.
 *
 * There is no difference between an added field or a new field. The
 * caller should check to see if the resulting field is as they require it.
 *
 * @param[in] tx The transaction with the memory pool and data collection.
 * @param[in] source Source to fetch or create.
 * @param[out] field The field found or created.
 * @returns
 * - IB_OK On success.
 * - Other on field creation or data field adding errors.
 */
static
ib_status_t get_or_create_field(
    ib_tx_t          *tx,
    ib_var_source_t  *source,
    ib_field_t      **field
)
{
    assert(tx != NULL);
    assert(tx->var_store != NULL);
    assert(source != NULL);
    assert(field != NULL);

    ib_status_t rc;

    /* Fetch field. */
    rc = ib_var_source_get(source, field, tx->var_store);
    /* Success. */
    if (rc == IB_OK) {
        return IB_OK;
    }
    /* Unexpected failure. */
    else if (rc != IB_ENOENT) {
        return rc;
    }

    /* Create a new, generic field. */
    rc = ib_field_create(
        field,
        tx->mm,
        "", 0, /* name will be set by ib_var_source_set() */
        IB_FTYPE_GENERIC,
        NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Add that field to the data collection. */
    rc = ib_var_source_set(source, tx->var_store, *field);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

/**
 * Set the target fields (FIELD, FIELD_TFN, FIELD_NAME, FIELD_NAME_FULL)
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] transformed Transformed value
 *
 * @returns Status code
 */
static ib_status_t set_target_fields(ib_rule_exec_t *rule_exec,
                                     const ib_field_t *transformed)
{
    assert(rule_exec != NULL);
    assert(rule_exec->tx != NULL);
    assert(rule_exec->tx->var_store != NULL);
    assert(rule_exec->value_stack != NULL);

    ib_status_t           rc = IB_OK;
    ib_tx_t              *tx = rule_exec->tx;
    ib_status_t           trc;
    ib_field_t           *fld_field;           /* The field FIELD. */
    ib_field_t           *fld_field_name;      /* The field FIELD_NAME. */
    ib_field_t           *fld_field_name_full; /* The field FIELD_NAME_FULL. */
    const ib_list_node_t *node;                /* Temporary list node. */
    size_t                namelen;             /* FIELD_NAME_FULL tmp value. */
    size_t                nameoff;             /* FIELD_NAME_FULL tmp value. */
    int                   names;               /* FIELD_NAME_FULL tmp value. */
    int                   n;                   /* FIELD_NAME_FULL tmp value. */
    char                 *name;                /* FIELD_NAME_FULL tmp value. */
    const ib_rule_engine_t *re = rule_exec->ib->rule_engine;

    if (! ib_flags_any(rule_exec->rule->flags, IB_RULE_FLAG_FIELDS)) {
        ib_rule_log_trace(rule_exec, "Not setting target fields");
        return IB_OK;
    }
    ib_rule_log_trace(rule_exec, "Creating target fields");

    /* The current value is the top of the stack */
    node = ib_list_last_const(rule_exec->value_stack);
    if ( (node == NULL) || (ib_list_node_data_const(node) == NULL) ) {
        return IB_OK;       /* Do nothing for now */
    }

    /* Get or create all fields. */
    trc = get_or_create_field(tx, re->source.field, &fld_field);
    if (trc != IB_OK) {
        ib_rule_log_error(
            rule_exec,
            "Failed to create FIELD: %s",
            ib_status_to_string(trc));
        rc = trc;
    }
    else {
        /* Shallow copy the field. */
        *fld_field = *(const ib_field_t *)ib_list_node_data_const(node);
    }

    /* Create FIELD_TFN */
    if (transformed != NULL) {
        ib_field_t *fld_field_tfn; /* The field FIELD_TFN. */
        trc = get_or_create_field(tx, re->source.field_tfn, &fld_field_tfn);
        if (trc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Failed to create FIELD_TFN: %s",
                              ib_status_to_string(trc));
            rc = trc;
        }
        else {
            /* Shallow copy the field. */
            *fld_field_tfn = *transformed;
        }
    }

    /* Create FIELD_TARGET */
    if (rule_exec->target != NULL) {
        ib_field_t *fld_field_target; /* The field FIELD_TARGET. */

        trc = get_or_create_field(tx, re->source.field_target, &fld_field_target);
        if (trc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Failed to create FIELD_TARGET: %s",
                              ib_status_to_string(trc));
            rc = trc;
        }
        else {
            ib_bytestr_t *bs;
            trc = ib_bytestr_dup_mem(
                &bs,
                tx->mm,
                (uint8_t *)(rule_exec->target->target_str),
                strlen(rule_exec->target->target_str)
            );
            if (trc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "Failed to set FIELD_TARGET: %s",
                                  ib_status_to_string(trc));
                rc = trc;
            }
            else {
                fld_field_target->type = IB_FTYPE_BYTESTR;
                trc = ib_field_setv(fld_field_target, ib_ftype_bytestr_in(bs));
                if (trc != IB_OK) {
                    ib_rule_log_error(rule_exec,
                                      "Failed to set FIELD_TARGET: %s",
                                      ib_status_to_string(trc));
                    rc = trc;
                }
            }
        }
    }

    /* Create FIELD_NAME */
    trc = get_or_create_field(tx, re->source.field_name, &fld_field_name);
    if (trc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Failed to create FIELD_NAME: %s",
                          ib_status_to_string(trc));
        rc = trc;
    }
    else {
        ib_bytestr_t *bs;
        trc = ib_bytestr_dup_mem(
            &bs,
            tx->mm,
            (uint8_t *)(fld_field->name),
            fld_field->nlen);
        if (trc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Failed to set FIELD_NAME: %s",
                              ib_status_to_string(trc));
            rc = trc;
        }
        else {
            fld_field_name->type = IB_FTYPE_BYTESTR;
            trc = ib_field_setv(fld_field_name, ib_ftype_bytestr_in(bs));
            if (trc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "Failed to set FIELD_NAME value: %s",
                                  ib_status_to_string(trc));
                rc = trc;
            }
        }
    }

    /* Create FIELD_NAME_FULL */

    /* Step 1: Calculate the buffer size & allocate */
    namelen = 0;
    names = 0;
    IB_LIST_LOOP_CONST(rule_exec->value_stack, node) {
        ib_field_t *fld_tmp = (ib_field_t *)ib_list_node_data_const(node);
        if (fld_tmp != NULL && fld_tmp->name != NULL && fld_tmp->nlen > 0) {
            ++names;
            if (fld_tmp->nlen > 0) {
                namelen += (fld_tmp->nlen + 1);
            }
        }
    }
    /* Remove space for the final ":" */
    if (namelen > 0) {
        --namelen;
    }
    name = ib_mm_alloc(tx->mm, namelen + 1);
    if (name == NULL) {
        return IB_EALLOC;
    }

    /* Step 2: Populate the name buffer. */
    nameoff = 0;
    n = 0;
    IB_LIST_LOOP_CONST(rule_exec->value_stack, node) {
        ib_field_t *fld_tmp = (ib_field_t *)ib_list_node_data_const(node);
        if (fld_tmp != NULL) {
            if (fld_tmp->nlen > 0) {
                memcpy(name+nameoff, fld_tmp->name, fld_tmp->nlen);
                nameoff += fld_tmp->nlen;
                ++n;
                if (n < names) {
                    *(name+nameoff) = ':';
                    ++nameoff;
                }
            }
        }
    }

    /* Step 3: Update the FIELD_NAME_FULL field. */
    trc = get_or_create_field(tx, re->source.field_name_full, &fld_field_name_full);
    if (trc != IB_OK) {
        ib_rule_log_error(
            rule_exec,
            "Failed to create FIELD_NAME_FULL: %s",
            ib_status_to_string(trc));
        rc = trc;
    }
    else {
        ib_bytestr_t *bs;
        trc = ib_bytestr_dup_mem(&bs, tx->mm, (uint8_t *)name, namelen);
        if (trc == IB_OK) {
            fld_field_name_full->type = IB_FTYPE_BYTESTR;
            rc = ib_field_setv(fld_field_name_full, ib_ftype_bytestr_in(bs));
        }
    }

    return rc;
}

/**
 * A debugging function that logs values at trace level.
 *
 * This function intentionally returns nothing because log level should
 * never impact the callers running.
 */
static void exe_op_trace_values(ib_rule_exec_t *rule_exec,
                                const ib_rule_operator_inst_t *opinst,
                                const ib_rule_target_t *target,
                                const ib_field_t *value)
{
    ib_status_t rc;

    if ( value == NULL ) {
        ib_rule_log_trace(rule_exec,
                          "Exec of op %s on target %s = NULL",
                          ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                          target->target_str);
    }
    else if (value->type == IB_FTYPE_NUM) {
        ib_num_t num;
        rc = ib_field_value(value, ib_ftype_num_out(&num));
        if ( rc != IB_OK ) {
            return;
        }
        ib_rule_log_trace(rule_exec,
                          "Exec of op %s on target %s = %" PRId64,
                          ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                          target->target_str,
                          num);
    }
    else if (value->type == IB_FTYPE_NULSTR) {
        const char* nulstr;
        const char *escaped;

        rc = ib_field_value(value, ib_ftype_nulstr_out(&nulstr));
        if (rc != IB_OK) {
            return;
        }

        escaped = ib_util_hex_escape(rule_exec->tx->mm,
                                     (const uint8_t *)nulstr,
                                     strlen(nulstr));
        if (escaped == NULL) {
            return;
        }

        ib_rule_log_trace(rule_exec,
                          "Exec of op %s on target %s = %s",
                          ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                          target->target_str,
                          escaped);
    }
    else if (value->type == IB_FTYPE_BYTESTR) {
        const char         *escaped;
        const ib_bytestr_t *bytestr;

        rc = ib_field_value(value, ib_ftype_bytestr_out(&bytestr));
        if ( rc != IB_OK ) {
            return;
        }

        escaped = ib_util_hex_escape(rule_exec->tx->mm,
                                     ib_bytestr_const_ptr(bytestr),
                                     ib_bytestr_size(bytestr));
        if (escaped == NULL) {
            return;
        }
        ib_rule_log_trace(rule_exec,
                          "Exec of op %s on target %s = %s",
                          ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                          target->target_str,
                          escaped);
    }
    else {
        ib_rule_log_trace(rule_exec,
                          "Exec of op %s on target %s = %s",
                          ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                          target->target_str,
                          "[cannot decode field type]");
    }
}

/**
 * Execute a phase rule operator on a list of values
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] value Field value to operate on
 * @param[in] recursion Recursion limit -- won't recurse if recursion is zero
 *
 * @returns Status code
 */
static ib_status_t execute_phase_operator(ib_rule_exec_t *rule_exec,
                                          const ib_field_t *value,
                                          int recursion)
{
    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(rule_exec->rule->opinst != NULL);
    assert(rule_exec->target != NULL);

    ib_status_t rc;
    const ib_rule_operator_inst_t *opinst = rule_exec->rule->opinst;
    const ib_rule_target_t   *target = rule_exec->target;

    /* This if-block is only to log operator values when tracing. */
    if (ib_rule_dlog_level(rule_exec->tx->ctx) >= IB_RULE_DLOG_TRACE) {
        exe_op_trace_values(rule_exec, opinst, target, value);
    }

    /* Limit recursion */
    --recursion;
    if (recursion <= 0) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: List recursion limit reached");
        return IB_EOTHER;
    }

    /* Handle a list by looping through it and recursively calling this func. */
    if ( (value != NULL) && (value->type == IB_FTYPE_LIST) ) {
        const ib_list_t      *vlist;
        const ib_list_node_t *node = NULL; /* Node in vlist. */
        int                   n    = 0;    /* Nth value in the vlist. */

        /* Fetch list out of value into vlist. */
        rc = ib_field_value(value, ib_ftype_list_out(&vlist));
        if (rc != IB_OK) {
            return rc;
        }

        /* Iterate over vlist. */
        IB_LIST_LOOP_CONST(vlist, node) {
            const ib_field_t *nvalue =
                (const ib_field_t *)ib_list_node_data_const(node);
            bool pushed;

            ++n;
            pushed = rule_exec_push_value(rule_exec, nvalue);

            /* Recursive call. */
            rc = execute_phase_operator(rule_exec, nvalue, recursion);
            if (rc != IB_OK) {
                ib_rule_log_warn(rule_exec,
                                 "Error executing list element #%d: %s",
                                 n, ib_status_to_string(rc));
            }
            rule_exec_pop_value(rule_exec, pushed);
        }

        ib_rule_log_debug(rule_exec, "Operator (list %zd) => %"PRId64,
                          vlist->nelts, rule_exec->rule_result);
    }

    /* No recursion required, handle it here */
    else {
        ib_num_t    result = 0;
        ib_status_t op_rc = IB_OK;

        /* Fill in the FIELD* fields */
        rc = set_target_fields(rule_exec, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Error creating one or more FIELD* fields: %s",
                              ib_status_to_string(rc));
        }

        {
            ib_list_node_t *node;
            IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.pre_operator, node) {
                const ib_rule_pre_operator_hook_t *hook =
                    (const ib_rule_pre_operator_hook_t *)
                        ib_list_node_data_const(node);
                hook->fn(
                    rule_exec,
                    opinst->opinst,
                    opinst->invert,
                    value,
                    hook->data
                );
            }
        }

        /* @todo remove the cast-away of the constness of value */
        op_rc = ib_operator_inst_execute(
            opinst->opinst,
            rule_exec->tx,
            (ib_field_t *)value,
            get_capture(rule_exec),
            &result
        );
        if (op_rc != IB_OK) {
            ib_rule_log_warn(rule_exec, "Operator returned an error: %s",
                             ib_status_to_string(op_rc));
        }

        {
            ib_list_node_t *node;
            IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.post_operator, node) {
                const ib_rule_post_operator_hook_t *hook =
                    (const ib_rule_post_operator_hook_t *)
                        ib_list_node_data_const(node);
                hook->fn(
                    rule_exec,
                    opinst->opinst,
                    opinst->invert,
                    value,
                    op_rc,
                    result,
                    get_capture(rule_exec),
                    hook->data
                );
            }
        }

        rc = ib_rule_log_exec_op(rule_exec->exec_log, opinst, op_rc);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Failed to log operator execution: %s",
                              ib_status_to_string(rc));
        }

        /* Store the results */
        store_results(rule_exec, value, op_rc, result);

        /* Execute any and all actions. */
        execute_rule_actions(rule_exec);

        /* Done. */
        clear_target_fields(rule_exec);
    }

    return rc;
}

/**
 * Execute an external rule.
 *
 * @param[in] rule_exec Rule execution environment.
 * @param[in] rule The rule object to execute.
 * @param[in] tx The current transaction.
 * @param[in] opinst The operator instance, that holds the external rule data.
 *
 * @return
 * - IB_OK On success.
 * - Other on rule failure.
 */
static ib_status_t execute_ext_phase_rule_targets(
    ib_rule_exec_t          *rule_exec,
    ib_rule_t               *rule,
    ib_tx_t                 *tx,
    ib_rule_operator_inst_t *opinst
)
{
    assert(rule_exec != NULL);
    assert(rule != NULL);
    assert(tx != NULL);
    assert(opinst != NULL);

    ib_status_t rc;
    ib_status_t op_rc;
    ib_num_t    result;

    /* Execute the operator */
    ib_rule_log_trace(rule_exec, "Executing external rule");
    op_rc = ib_operator_inst_execute(
        opinst->opinst,
        rule_exec->tx,
        NULL,
        get_capture(rule_exec),
        &result
    );
    rule_exec->rule_result = result;
    rule_exec->cur_result  = result;
    if (op_rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "External operator returned an error: %s",
                          ib_status_to_string(op_rc));
    }
    rc = ib_rule_log_exec_op(rule_exec->exec_log, opinst, op_rc);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Failed to log external operator execution: %s",
                          ib_status_to_string(rc));
    }
    ib_rule_log_execution(rule_exec);
    return rc;
}

/**
 * Execute a single rule's operator on all target fields.
 *
 * @param[in] rule_exec Rule execution object
 *
 * @returns Status code
 */
static ib_status_t execute_phase_rule_targets(ib_rule_exec_t *rule_exec)
{
    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(rule_exec->rule->opinst != NULL);
    assert(rule_exec->rule->opinst->opinst != NULL);
    assert(rule_exec->tx != NULL);

    ib_tx_t                 *tx     = rule_exec->tx;
    ib_rule_t               *rule   = rule_exec->rule;
    ib_rule_operator_inst_t *opinst = rule_exec->rule->opinst;
    ib_status_t              rc     = IB_OK;
    ib_list_node_t          *node   = NULL;

    /* Special case: External rules */
    if (ib_flags_all(rule->flags, IB_RULE_FLAG_EXTERNAL)) {
        return execute_ext_phase_rule_targets(rule_exec, rule, tx, opinst);
    }

    /* Log what we're going to do */
    ib_rule_log_debug(rule_exec, "Executing rule");

    /* If this is a no-target rule (i.e. action), do nothing */
    if (ib_flags_all(rule->flags, IB_RULE_FLAG_NO_TGT)) {
        assert(ib_list_elements(rule->target_fields) == 1);
    }
    else {
        assert(ib_list_elements(rule->target_fields) != 0);
    }

    ib_rule_log_debug(rule_exec, "Operating on %zd fields.",
                      ib_list_elements(rule->target_fields));

    /* Loop through all of the fields.
     *
     * @todo The current behavior is to keep running even after an operator
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP(rule->target_fields, node) {
        ib_status_t         getrc;
        ib_list_t          *result     = NULL;
        ib_field_t         *value      = NULL; /* Var. */
        const ib_field_t   *tfnvalue   = NULL; /* Var value after tfns */
        bool                pushed     = true;
        bool                pop_target = false;
        ib_rule_target_t   *target     =
            (ib_rule_target_t *)ib_list_node_data(node);

        assert(target != NULL);

        /* Set the target in the rule execution object */
        rule_exec->target = target;

        /* Get the field value */
        /* The const cast below is unfortunate, but we currently don't have a
         * good way of expressing const-list-fields.  The list will not be
         * modified below.
         */
        if (target->target == NULL) {
            getrc = IB_ENOENT;
        }
        else {
            getrc = ib_var_target_get(
                target->target,
                (const ib_list_t **)(&result),
                tx->mm,
                tx->var_store
            );
        }
        if (getrc == IB_ENOENT) {
            bool allow = ib_flags_all(
                ib_operator_capabilities(ib_operator_inst_operator(opinst->opinst)),
                IB_OP_CAPABILITY_ALLOW_NULL
            );

            if (! allow) {
                ib_rule_log_debug(rule_exec,
                                  "Operator %s will not execute because "
                                  "there is no target %s.",
                                  ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                                  target->target_str);
                ib_rule_log_exec_add_target(rule_exec->exec_log, target, NULL);
                continue;
            }

            ib_rule_log_debug(rule_exec,
                              "Operator %s receiving null argument because "
                              "there is no target %s.",
                              ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                              target->target_str);
        }
        else if (getrc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error getting target field: %s",
                              ib_status_to_string(rc));
            ib_rule_log_exec_add_target(rule_exec->exec_log, target, NULL);
            continue;
        }

        if (result != NULL) {
            /* If list has one element, pull out into value.  Otherwise,
             * wrap in an unnamed list field.
             */
            if (ib_list_elements(result) == 1) {
                value = (ib_field_t *)ib_list_node_data(ib_list_first(result));
            }
            else {
                rc = ib_field_create(
                    &value,
                    tx->mm,
                    IB_S2SL(""),
                    IB_FTYPE_LIST,
                    ib_ftype_list_in(result)
                );
                if (rc != IB_OK) {
                    ib_rule_log_error(rule_exec, "Failed to wrap results: %s",
                                      ib_status_to_string(rc));
                    continue;
                }
            }
            assert(value != NULL);
        }

        /* Add the target to the log object */
        ib_rule_log_exec_add_target(rule_exec->exec_log, target, value);

        /* If there is a defined target, store it's name (as a field)
         * in the value_stack of the rule_exec object.
         *
         * This allows FIELD_NAME_FULL to be correctly populated.
         */
        if (target != NULL && target->target != NULL) {
            ib_ftype_t type;

            rc = ib_var_target_type(target->target, tx->var_store, &type);
            if (rc == IB_OK && type == IB_FTYPE_LIST) {
                ib_field_t *target_field;
                const char *target_name     = "";   /* Name of target. */
                size_t      target_name_len = 0;    /* Len of target_name. */

                ib_var_target_source_name(
                    target->target,
                    &target_name,
                    &target_name_len);

                rc = ib_field_create(
                    &target_field,
                    tx->mm,
                    target_name,
                    target_name_len,
                    IB_FTYPE_GENERIC,
                    NULL);
                if (rc != IB_OK) {
                    return rc;
                }

                /* Add the target to the FIELD_NAME_FULL stack. */
                if (!rule_exec_push_value(rule_exec, target_field)) {
                    ib_log_error_tx(tx, "Failed to record target name.");
                    return IB_EOTHER;
                }

                pop_target = true;
            }
        }

        /* Execute the target transformations */
        if (value != NULL) {
            rc = execute_tfns(rule_exec, value, &tfnvalue);
            if (rc != IB_OK) {
                return rc;
            }
        }

        /* Store the rule's final value */
        ib_rule_log_exec_set_tgt_final(rule_exec->exec_log, tfnvalue);

        /* Put the value on the value stack */
        pushed = rule_exec_push_value(rule_exec, value);

        /* Execute the rule operator on the value. */
        if ( (tfnvalue != NULL) && (tfnvalue->type == IB_FTYPE_LIST) ) {
            ib_list_t *value_list;
            ib_list_node_t *value_node;
            rc = ib_field_value(tfnvalue, (void *)&value_list);

            if (rc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "Error getting target field value: %s",
                                  ib_status_to_string(rc));
                continue;
            }

            /* Log when there are no arguments. */
            if ( (ib_list_elements(value_list) == 0) &&
                 (ib_rule_dlog_level(tx->ctx) >= IB_RULE_DLOG_TRACE) ) {
                ib_rule_log_trace(rule_exec,
                                  "Rule not running because there are no "
                                  "values for operator %s "
                                  "to operate on in target %s.",
                                  ib_operator_name(ib_operator_inst_operator(opinst->opinst)),
                                  target->target_str);
            }

            /* Run operations on each list element. */
            IB_LIST_LOOP(value_list, value_node) {
                ib_field_t *node_value = (ib_field_t *)
                    ib_list_node_data(value_node);
                bool lpushed;

                lpushed = rule_exec_push_value(rule_exec, node_value);

                rc = execute_phase_operator(rule_exec, node_value,
                                            MAX_LIST_RECURSION);
                if (rc != IB_OK) {
                    ib_rule_log_error(rule_exec,
                                      "Operator returned an error: %s",
                                      ib_status_to_string(rc));
                    return rc;
                }
                ib_rule_log_trace(rule_exec, "Operator result => %" PRId64,
                                  rule_exec->rule_result);
                rule_exec_pop_value(rule_exec, lpushed);
            }
        }
        else {
            ib_rule_log_trace(rule_exec, "Calling exop on single target");
            rc = execute_phase_operator(rule_exec, tfnvalue, MAX_LIST_RECURSION);
            if (rc != IB_OK) {
                ib_rule_log_error(rule_exec,
                                  "Operator returned an error: %s",
                                  ib_status_to_string(rc));

                /* Clean up the value stack before we return */
                rule_exec_pop_value(rule_exec, pushed);
                return rc;
            }

            /* Log it */
            ib_rule_log_trace(rule_exec, "Operator result => %" PRId64,
                              rule_exec->cur_result);
        }

        /* Pop this element off the value stack */
        rule_exec_pop_value(rule_exec, pushed);

        /* Pop the target value if it was pushed. */
        rule_exec_pop_value(rule_exec, pop_target);
    }

    ib_rule_log_execution(rule_exec);

    return rc;
}

/**
 * Execute a single phase rule, it's actions, and it's chained rules.
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] rule Rule to execute
 * @param[in] recursion Recursion limit
 *
 * @returns Status code
 */
static ib_status_t execute_phase_rule(ib_rule_exec_t *rule_exec,
                                      const ib_rule_t *rule,
                                      int recursion)
{
    ib_status_t         rc = IB_OK;
    ib_status_t         trc;          /* Temporary status code */
#ifdef IB_RULE_TRACE
    ib_time_t pre_time;
    ib_time_t post_time;
#endif

    assert(rule_exec != NULL);
    assert(rule != NULL);
    assert(! rule->phase_meta->is_stream);

    --recursion;
    if (recursion <= 0) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Phase chain recursion limit reached");
        return IB_EOTHER;
    }

    {
        ib_list_node_t *node;
        IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.pre_rule, node) {
            const ib_rule_pre_rule_hook_t *hook =
                (const ib_rule_pre_rule_hook_t *)
                    ib_list_node_data_const(node);
            hook->fn(rule_exec, hook->data);
        }
    }

    /* Set the rule in the execution object */
    rc = rule_exec_push_rule(rule_exec, rule);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: "
                          "Failed to set rule in execution object: %s",
                          ib_status_to_string(rc));
        return rc;
    }

    /*
     * Execute the rule operator on the target fields.
     *
     * @todo The current behavior is to keep running even after an operator
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
#ifdef IB_RULE_TRACE
    if (rule->flags & IB_RULE_FLAG_TRACE) {
        pre_time = ib_clock_get_time();
    }
#endif
    trc = execute_phase_rule_targets(rule_exec);
    if (trc != IB_OK) {
        rc = trc;
        goto cleanup;
    }
#ifdef IB_RULE_TRACE
    if (rule->flags & IB_RULE_FLAG_TRACE) {
        post_time = ib_clock_get_time();
        rule_exec->traces[rule->meta.index].rule = rule;
        rule_exec->traces[rule->meta.index].evaluation_time +=
            (post_time - pre_time);
        ++rule_exec->traces[rule->meta.index].evaluation_n;
    }
#endif

    /*
     * Execute chained rule
     *
     * @todo The current behavior is to keep running even after a chained rule
     * returns an error.  This needs further discussion to determine what
     * the correct behavior should be.
     *
     * @note Chaining is currently done via recursion.
     */
    if ( (rule_exec->rule_result != 0) && (rule->chained_rule != NULL) ) {
        ib_rule_log_debug(rule_exec,
                          "Chaining to rule \"%s\"",
                          ib_rule_id(rule->chained_rule));
        trc = execute_phase_rule(rule_exec, rule->chained_rule, recursion);

        if (trc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Error executing chained rule \"%s\": %s",
                              ib_rule_id(rule->chained_rule),
                              ib_status_to_string(trc));
            rc = trc;
        }
    }

    /* Pop the rule from the execution object */
cleanup:
    trc = rule_exec_pop_rule(rule_exec);
    if (trc != IB_OK) {
        /* Do nothing */
    }

    {
        ib_list_node_t *node;
        IB_LIST_LOOP(rule_exec->ib->rule_engine->hooks.post_rule, node) {
            const ib_rule_post_rule_hook_t *hook =
                (const ib_rule_post_rule_hook_t *)
                    ib_list_node_data_const(node);
            hook->fn(rule_exec, hook->data);
        }
    }

    return rc;
}

/**
 * Check if the current rule is runnable
 *
 * @param[in] ctx_rule Context rule data
 *
 * @returns true if the rule is runnable, otherwise false
 */
static bool rule_is_runnable(const ib_rule_ctx_data_t *ctx_rule)
{
    /* Skip invalid / disabled rules */
    if (! ib_flags_all(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED)) {
        return false;
    }
    if (! ib_flags_all(ctx_rule->rule->flags, IB_RULE_FLAG_VALID)) {
        return false;
    }

    return true;
}

/**
 * Check if allow affects the current rule
 *
 * @param[in] tx Transaction
 * @param[in] meta Rule's phase meta-data
 * @param[in] check_phase Check if ALLOW_PHASE is set
 *
 * @returns true if the rule is affected, otherwise false
 */
static bool rule_allow(const ib_tx_t *tx,
                       const ib_rule_phase_meta_t *meta,
                       bool check_phase)
{
    /* Check if the transaction was already blocked. */
    if (ib_tx_is_blocked(tx)) {
        return false;
    }

    /* Check the ALLOW_ALL flag */
    if ( (meta->phase_num != IB_PHASE_POSTPROCESS) &&
         (meta->phase_num != IB_PHASE_LOGGING) &&
         (ib_flags_all(tx->flags, IB_TX_FALLOW_ALL) == 1) )
    {
        ib_log_debug_tx(tx,
                        "Skipping phase %d/\"%s\" in context \"%s\": "
                        "ALLOW_ALL set.",
                        meta->phase_num, meta->name,
                        ib_context_full_get(tx->ctx));
        return true;
    }

    /* If this is a request phase rule, Check the ALLOW_REQUEST flag */
    if ( (ib_flags_all(meta->flags, PHASE_FLAG_REQUEST)) &&
         (ib_flags_all(tx->flags, IB_TX_FALLOW_REQUEST) == 1) )
    {
        ib_log_debug_tx(tx,
                        "Skipping phase %d/\"%s\" in context \"%s\": "
                        "ALLOW_REQUEST set.",
                        meta->phase_num, meta->name,
                        ib_context_full_get(tx->ctx));
        return true;
    }

    /* If check_phase is true, check the ALLOW_PHASE flag */
    if ( check_phase && (ib_flags_all(tx->flags, IB_TX_FALLOW_PHASE) == 1) )
    {
        ib_log_debug_tx(tx,
                        "Skipping remaining rules phase %d/\"%s\" "
                        "in context \"%s\": ALLOW_PHASE set.",
                        meta->phase_num, meta->name,
                        ib_context_full_get(tx->ctx));
        return true;
    }

    return false;
}

/**
 * Inject rules into the rule execution object's phase rule list
 *
 * @param[in] ib IronBee engine
 * @param[in] phase_meta Phase meta data
 * @param[in,out] rule_exec Rule execution object
 *
 * @returns Status code
 */
static ib_status_t inject_rules(const ib_engine_t *ib,
                                const ib_rule_phase_meta_t *phase_meta,
                                ib_rule_exec_t *rule_exec)
{
    assert(ib != NULL);
    assert(phase_meta != NULL);
    assert(rule_exec != NULL);

    const ib_list_node_t *node;
    const ib_list_t      *injection_cbs;
    size_t                rule_count = 0;   /* Used only for trace debugging */
    ib_rule_phase_num_t   phase = phase_meta->phase_num;

    injection_cbs = ib->rule_engine->injection_cbs[phase];
    if (injection_cbs == NULL) {
        return IB_OK;
    }

    IB_LIST_LOOP_CONST(injection_cbs, node) {
        const ib_rule_injection_cb_t *cb =
            (const ib_rule_injection_cb_t *)ib_list_node_data_const(node);
        const ib_list_node_t *rule_node;
        ib_status_t rc;
        int invalid_count = 0;

        rc = cb->fn(ib, rule_exec, rule_exec->phase_rules, cb->data);
        if (rc != IB_OK) {
            ib_rule_log_tx_error(rule_exec->tx,
                                 "Rule engine: Rule injector \"%s\" "
                                 "for phase %d/\"%s\" returned %s",
                                 cb->name,
                                 phase, phase_name(phase_meta),
                                 ib_status_to_string(rc));
            return rc;
        }

        /* Verify that all of the injected rules have the correct phase.
         * Because this check is O(n^2), only do this if rule logging is set
         * to DEBUG or higher. */
        if (ib_rule_dlog_level(rule_exec->tx->ctx) >= IB_RULE_DLOG_DEBUG) {
            IB_LIST_LOOP_CONST(rule_exec->phase_rules, rule_node) {
                const ib_rule_t *rule =
                    (const ib_rule_t *)ib_list_node_data_const(rule_node);
                if (
                    rule->meta.phase != IB_PHASE_NONE &&
                    rule->meta.phase != phase
                ) {
                    ib_rule_log_tx_error(
                        rule_exec->tx,
                        "Rule injector \"%s\" for phase %d/\"%s\" "
                        "injected rule \"%s\" for incorrect phase %d",
                        cb->name, phase, phase_name(phase_meta),
                        ib_rule_id(rule), rule->meta.phase);
                    ++invalid_count;
                }
            }
            if (invalid_count != 0) {
                return IB_EINVAL;
            }
        }

        /* Debug logging */
        if (ib_rule_dlog_level(rule_exec->tx->ctx) >= IB_RULE_DLOG_TRACE) {
            size_t new_count = ib_list_elements(rule_exec->phase_rules);
            ib_rule_log_tx_trace(rule_exec->tx,
                                 "Rule injector \"%s\" for phase %d/\"%s\" "
                                 "injected %zd rules\n",
                                 cb->name,
                                 phase, phase_name(phase_meta),
                                 new_count - rule_count);
            rule_count = new_count;
        }
    }
    return IB_OK;
}

/**
 * Append context rules onto the rule execution object's phase rule list
 *
 * @param[in] ib IronBee engine
 * @param[in] phase_meta Phase meta data
 * @param[in] rule_list List of context rules to append
 * @param[in,out] rule_exec Rule execution object
 *
 * @returns Status code
 */
static ib_status_t append_context_rules(const ib_engine_t *ib,
                                        const ib_rule_phase_meta_t *phase_meta,
                                        const ib_list_t *rule_list,
                                        ib_rule_exec_t *rule_exec)
{
    assert(ib != NULL);
    assert(phase_meta != NULL);
    assert(rule_exec != NULL);
    assert(rule_list != NULL);

    const ib_list_node_t *node;

    IB_LIST_LOOP_CONST(rule_list, node) {
        const ib_rule_ctx_data_t *ctx_rule =
            (const ib_rule_ctx_data_t *)ib_list_node_data_const(node);

        if (rule_is_runnable(ctx_rule)) {
            ib_list_push(rule_exec->phase_rules, ctx_rule->rule);
        }
    }

    return IB_OK;
}

/**
 * Run a set of phase rules.
 *
 * @param[in] ib Engine.
 * @param[in,out] tx Transaction.
 * @param[in] state State.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t).
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_DECLINED if the httpd plugin declines to block the traffic.
 */
static ib_status_t run_phase_rules(ib_engine_t *ib,
                                   ib_tx_t *tx,
                                   ib_state_t state,
                                   void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->ctx != NULL);
    assert(cbdata != NULL);

    /* If this is an empty request, ignore the transaction */
    if (! ib_flags_any(tx->flags, IB_TX_FREQ_HAS_DATA | IB_TX_FRES_HAS_DATA)) {
        return IB_OK;
    }

    /* The rule execution object isn't created if tx_started never notified.
     * This can happen if a connection is created to ATS, but no data
     * is actually pushed through the connection. */
    if (tx->rule_exec == NULL) {
        if (! ib_flags_all(tx->flags, IB_TX_FREQ_STARTED) ) {
            return IB_OK;
        }
        ib_log_alert_tx(tx, "Rule execution object not created @ %s",
                        ib_state_name(state));
        return IB_EUNKNOWN;
    }

    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;
    ib_context_t               *ctx = tx->ctx;
    const ib_ruleset_phase_t   *ruleset_phase;
    ib_rule_exec_t             *rule_exec = tx->rule_exec;
    const ib_list_t            *rules;
    const ib_list_node_t       *node = NULL;
    ib_status_t                 rc = IB_OK;

    ruleset_phase = &(ctx->rules->ruleset.phases[meta->phase_num]);
    assert(ruleset_phase != NULL);
    rules = ruleset_phase->rule_list;
    assert(rules != NULL);

    /* Log the transaction event start */
    ib_rule_log_tx_event_start(rule_exec, state);
    ib_rule_log_phase(rule_exec,
                      meta->phase_num, phase_name(meta),
                      ib_list_elements(rules));

    /* Check if this phase should be skipped. */
    if (rule_allow(tx, meta, true)) {
        rc = IB_OK;
        goto finish;
    }

    /* If we're blocking, skip processing */
    if (ib_flags_any(tx->flags, IB_TX_FBLOCK_PHASE | IB_TX_FBLOCK_IMMEDIATE) &&
        (ib_flags_any(meta->flags, PHASE_FLAG_FORCE) == false) )
    {
        /* Report blocking. */
        rc = ib_tx_block(rule_exec->tx);
        if (rc != IB_OK && rc != IB_DECLINED) {
            ib_rule_log_error(rule_exec, "Failed to block: %s",
                              ib_status_to_string(rc));
        }
        else {
            if (rc == IB_DECLINED) {
                ib_rule_log_info(rule_exec, "Declined to block.");
            }
            ib_log_debug_tx(tx,
                            "Not executing rules for phase %d/\"%s\" "
                            "in context \"%s\": transaction was blocked.",
                            meta->phase_num, phase_name(meta),
                            ib_context_full_get(ctx));
            rc = IB_OK;
            goto finish;
        }
    }

    /* Clear the phase allow flag since we are processing a new phase. */
    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);

    /* Sanity check */
    if (ruleset_phase->phase_num != meta->phase_num) {
        ib_rule_log_tx_error(tx,
                             "Rule engine: Phase %d/\"%s\" is %d",
                             meta->phase_num, phase_name(meta),
                             ruleset_phase->phase_num);
        rc = IB_EINVAL;
        goto finish;
    }

    /* Setup for rule execution */
    rule_exec->phase = meta->phase_num;
    rule_exec->is_stream = false;
    ib_list_clear(rule_exec->phase_rules);

    /* Invoke all of the rule injectors */
    rc = inject_rules(ib, meta, rule_exec);
    if (rc != IB_OK) {
        return IB_EINVAL;
    }

    /* Add all of the enabled "normal" rules to the list */
    rc = append_context_rules(ib, meta, rules, rule_exec);
    if (rc != IB_OK) {
        return IB_EINVAL;
    }

    /* Walk through the rules & execute them */
    if (ib_list_elements(rule_exec->phase_rules) == 0) {
        ib_rule_log_tx_debug(tx,
                             "No rules for phase %d/\"%s\" in context \"%s\"",
                             meta->phase_num, phase_name(meta),
                             ib_context_full_get(ctx));
        rc = IB_OK;
        goto finish;
    }
    ib_rule_log_tx_debug(tx,
                         "Executing %zd rules for phase %d/\"%s\" "
                         "in context \"%s\"",
                         ib_list_elements(rule_exec->phase_rules),
                         meta->phase_num, phase_name(meta),
                         ib_context_full_get(ctx));

    /*
     * Loop through all of the rules for this phase, execute them.
     *
     * @todo The current behavior is to keep running even after rule execution
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP_CONST(rule_exec->phase_rules, node) {
        const ib_rule_t *rule =
            (const ib_rule_t *)ib_list_node_data_const(node);
        ib_status_t      rule_rc;

        assert(
            rule->meta.phase == meta->phase_num ||
            rule->meta.phase == IB_PHASE_NONE
        );

        /* Allow (skip) this phase? */
        if (rule_allow(tx, meta, true)) {
            break;
        }

        /* Execute the rule, it's actions and chains */
        rule_rc = execute_phase_rule(rule_exec, rule, MAX_CHAIN_RECURSION);

        /* Handle declined return code. Did this block? */
        if (ib_flags_all(tx->flags, IB_TX_FBLOCK_IMMEDIATE) ) {
            bool block_rc;
            ib_rule_log_debug(rule_exec,
                              "Rule resulted in immediate block "
                              "(aborting rule processing): %s",
                              ib_status_to_string(rule_rc));
            block_rc = ib_tx_block(rule_exec->tx);
            if (block_rc != IB_OK && block_rc != IB_DECLINED) {
                ib_rule_log_error(rule_exec, "Failed to block: %s",
                                  ib_status_to_string(block_rc));
                if (rule_rc == IB_OK) {
                    rule_rc = block_rc;
                }
            }
            else {
                if (block_rc == IB_DECLINED) {
                    ib_rule_log_info(rule_exec, "Declined to block.");
                }
                goto finish;
            }
        }
        if ( (rc == IB_OK) && (rule_rc != IB_OK) ) {
            rc = rule_rc;
        }
    }

    if (ib_flags_all(tx->flags, IB_TX_FBLOCK_PHASE) ) {
        ib_rule_log_tx_debug(tx, "Rule(s) resulted in phase block");
        rc = ib_tx_block(rule_exec->tx);
        if (rc == IB_DECLINED) {
            ib_rule_log_info(rule_exec, "Declined to block.");
            rc = IB_OK;
        }
        else if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Failed to block phase: %s",
                                 ib_status_to_string(rc));
        }
    }

    /* Log the end of the tx event */
finish:
    ib_rule_log_tx_event_end(rule_exec, state);

    /* Clear the phase allow flag. */
    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);

    /*
     * @todo Eat errors for now.  Unless something Really Bad(TM) has
     * occurred, return IB_OK to the engine.  A bigger discussion of if / how
     * such errors should be propagated needs to occur.
     */
    return rc;
}

/**
 * Execute a single stream operator, and it's actions
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] value Value to pass to the operator
 *
 * @returns Status code
 */
static ib_status_t execute_stream_operator(ib_rule_exec_t *rule_exec,
                                           ib_field_t *value)
{
    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(value != NULL);

    ib_status_t      rc;
    const ib_rule_t *rule = rule_exec->rule;
    bool             pushed = rule_exec_push_value(rule_exec, value);
    ib_num_t         result = 0;
    ib_status_t      op_rc;

    /* Add a target execution result to the log object */
    ib_rule_log_exec_add_stream_tgt(rule_exec->ib, rule_exec->exec_log, value);

    /* Fill in the FIELD* fields */
    rc = set_target_fields(rule_exec, value);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Error creating one or more FIELD* fields: %s",
                          ib_status_to_string(rc));
    }

    /* Execute the rule operator */
    op_rc = ib_operator_inst_execute(
        rule->opinst->opinst,
        rule_exec->tx,
        value,
        get_capture(rule_exec),
        &result
    );
    if (op_rc != IB_OK) {
        ib_rule_log_error(rule_exec, "Operator returned an error: %s",
                          ib_status_to_string(op_rc));
        return op_rc;
    }
    rc = ib_rule_log_exec_op(rule_exec->exec_log, rule->opinst, rc);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec, "Failed to log operator execution: %s",
                          ib_status_to_string(rc));
    }
    ib_rule_log_trace(rule_exec, "Operator => %" PRId64, result);

    rule_exec_pop_value(rule_exec, pushed);

    /* Store the results */
    store_results(rule_exec, value, op_rc, result);

    /* Execute any and all actions. */
    execute_rule_actions(rule_exec);

    /* Block if required */
    if (ib_flags_all(rule_exec->tx->flags, IB_TX_FBLOCK_IMMEDIATE) ) {
        ib_rule_log_debug(rule_exec,
                          "Rule resulted in immediate block "
                          "(aborting rule processing): %s",
                          ib_status_to_string(rc));
        rc = ib_tx_block(rule_exec->tx);
        if (rc == IB_DECLINED) {
            ib_rule_log_info(rule_exec, "Declined to block.");
            rc = IB_OK;
        }
        else if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Failed to block: %s",
                              ib_status_to_string(rc));
        }
    }

    ib_rule_log_execution(rule_exec);
    clear_target_fields(rule_exec);

    return rc;
}

/**
 * Execute a single stream txdata rule, and it's actions
 *
 * @param[in] rule_exec   Rule execution object
 * @param[in] data        Transaction data.
 * @param[in] data_length Length of @a data.
 *
 * @returns Status code
 */
static
ib_status_t execute_stream_txdata_rule(
    ib_rule_exec_t *rule_exec,
    const char     *data,
    size_t          data_length
)
{
    ib_status_t    rc = IB_OK;
    ib_field_t    *value = NULL;

    assert(rule_exec != NULL);
    assert(rule_exec->rule != NULL);
    assert(rule_exec->rule->phase_meta->is_stream);
    assert(data != NULL);


    /*
     * Execute the rule operator.
     *
     * @todo The current behavior is to keep running even after action(s)
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */

    /* Create a field to hold the data */
    rc = ib_field_create_bytestr_alias(&value,
                                       rule_exec->tx->mm,
                                       "txdata", 3,
                                       (const uint8_t *)data, data_length);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Error creating field for stream "
                          "txdata rule data: %s",
                          ib_status_to_string(rc));
        return rc;
    }

    rc = execute_stream_operator(rule_exec, value);

    return rc;
}

/**
 * Execute a single stream header rule, and it's actions
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] header Parsed header
 *
 * @returns Status code
 */
static ib_status_t execute_stream_header_rule(ib_rule_exec_t *rule_exec,
                                              ib_parsed_header_t *header)
{
    ib_status_t          rc = IB_OK;
    ib_field_t          *value;
    ib_parsed_header_t *nvpair;

    assert(rule_exec != NULL);
    assert(header != NULL);

    /*
     * Execute the rule operator.
     *
     * @todo The current behavior is to keep running even after action(s)
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    for (nvpair = header;  nvpair != NULL;  nvpair = nvpair->next) {

        /* Create a field to hold the data */
        ib_rule_log_debug(rule_exec,
                          "nvpair: \"%.*s\"=\"%.*s\"\n",
                          (int)ib_bytestr_length(nvpair->name),
                          (const char *)ib_bytestr_const_ptr(nvpair->name),
                          (int)ib_bytestr_length(nvpair->value),
                          (const char *)ib_bytestr_const_ptr(nvpair->value));
        rc = ib_field_create(&value,
                             rule_exec->tx->mm,
                             (const char *)ib_bytestr_const_ptr(nvpair->name),
                             ib_bytestr_length(nvpair->name),
                             IB_FTYPE_BYTESTR,
                             ib_ftype_bytestr_in(nvpair->value));
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec,
                              "Error creating field for "
                              "header stream rule data: %s",
                              ib_status_to_string(rc));
            return rc;
        }

        rc = execute_stream_operator(rule_exec, value);
    }

    return rc;
}

/**
 * Run a set of stream rules.
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] data Transaction data (or NULL)
 * @param[in] data_length Length of @a data.
 * @param[in] header Parsed header (or NULL)
 * @param[in] meta Phase meta data
 *
 * @returns
 *   - Status code IB_OK
 */
static ib_status_t run_stream_rules(ib_engine_t *ib,
                                    ib_tx_t *tx,
                                    ib_state_t state,
                                    const char *data,
                                    size_t data_length,
                                    ib_parsed_header_t *header,
                                    const ib_rule_phase_meta_t *meta)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->rule_exec != NULL);
    assert( (data != NULL) || (header != NULL) );
    assert(meta != NULL);
    assert( (meta->hook_type != IB_STATE_HOOK_TXDATA) || (data != NULL) );
    assert( (meta->hook_type != IB_STATE_HOOK_HEADER) || (header != NULL) );

    ib_context_t             *ctx = tx->ctx;
    const ib_ruleset_phase_t *ruleset_phase =
        &(ctx->rules->ruleset.phases[meta->phase_num]);
    ib_list_t                *rules = ruleset_phase->rule_list;
    const ib_list_node_t     *node = NULL;
    ib_rule_exec_t           *rule_exec = tx->rule_exec;
    ib_status_t               rc;

    /* Log the transaction event start */
    ib_rule_log_tx_event_start(rule_exec, state);
    ib_rule_log_phase(rule_exec,
                      meta->phase_num, phase_name(meta),
                      ib_list_elements(rules));

    /* Allow (skip) this phase? Perhaps the whole TX is allowed? */
    if (rule_allow(tx, meta, false)) {
        return IB_OK;
    }

    /* Sanity check */
    if (ruleset_phase->phase_num != meta->phase_num) {
        ib_rule_log_error(rule_exec,
                          "Rule engine: Stream %d/\"%s\" is %d",
                          meta->phase_num, phase_name(meta),
                          ruleset_phase->phase_num);
        return IB_EINVAL;
    }

    /* Setup for rule execution */
    rule_exec->phase = meta->phase_num;
    rule_exec->is_stream = true;
    ib_list_clear(rule_exec->phase_rules);

    /* Invoke all of the rule injectors */
    rc = inject_rules(ib, meta, rule_exec);
    if (rc != IB_OK) {
        return IB_EINVAL;
    }

    /* Add all of the enabled "normal" rules to the list */
    rc = append_context_rules(ib, meta, rules, rule_exec);
    if (rc != IB_OK) {
        return IB_EINVAL;
    }

    /* Are there any rules?  If not, do a quick exit */
    if (ib_list_elements(rule_exec->phase_rules) == 0) {
        ib_rule_log_debug(rule_exec,
                          "No rules for stream %d/\"%s\" in context \"%s\"",
                          meta->phase_num, phase_name(meta),
                          ib_context_full_get(ctx));
        return IB_OK;
    }
    ib_rule_log_debug(rule_exec,
                      "Executing %zd rules for stream %d/\"%s\" "
                      "in context \"%s\"",
                      ib_list_elements(rule_exec->phase_rules),
                      meta->phase_num, phase_name(meta),
                      ib_context_full_get(ctx));

    /*
     * Loop through all of the rules for this phase, execute them.
     *
     * @todo The current behavior is to keep running even after rule execution
     * returns an error.  This needs further discussion to determine what the
     * correct behavior should be.
     */
    IB_LIST_LOOP_CONST(rule_exec->phase_rules, node) {
        const ib_rule_t    *rule =
            (const ib_rule_t *)ib_list_node_data_const(node);
        ib_status_t         trc;

        /* Reset status */
        rc = IB_OK;

        /* Allow (skip) this phase? */
        if (rule_allow(tx, meta, true)) {
            break;
        }

        /* Push onto the rule execution stack */
        trc = rule_exec_push_rule(rule_exec, rule);
        if (trc != IB_OK) {
            break;
        }

        /*
         * Execute the rule
         *
         * @todo The current behavior is to keep running even after an
         * operator returns an error.  This needs further discussion to
         * determine what the correct behavior should be.
         */
        if (data != NULL) {
            rc = execute_stream_txdata_rule(rule_exec, data, data_length);
        }
        else if (header != NULL) {
            rc = execute_stream_header_rule(rule_exec, header);
        }
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error executing rule: %s",
                              ib_status_to_string(rc));
        }

        ib_rule_log_execution(rule_exec);
        rc = rule_exec_pop_rule(rule_exec);
        if (rc != IB_OK) {
            break;
        }
    }

    if (ib_flags_all(tx->flags, IB_TX_FBLOCK_PHASE) ) {
        rc = ib_tx_block(rule_exec->tx);
        if (rc == IB_DECLINED) {
            ib_rule_log_info(rule_exec, "Declined to block.");
        }
        else if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Failed to block: %s",
                              ib_status_to_string(rc));
        }
    }

    /*
     * @todo Eat errors for now.  Unless something Really Bad(TM) has
     * occurred, return IB_OK to the engine.  A bigger discussion of if / how
     * such errors should be propagated needs to occur.
     */
    rule_exec->phase = IB_PHASE_NONE;
    return IB_OK;
}

/**
 * Run stream header rules
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] header Parsed header
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_header_rules(ib_engine_t *ib,
                                           ib_tx_t *tx,
                                           ib_state_t state,
                                           ib_parsed_header_t *header,
                                           void *cbdata)
{
    assert(ib != NULL);
    assert(cbdata != NULL);

    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;
    ib_status_t rc = IB_OK;

    if (header != NULL) {
        ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);
        rc = run_stream_rules(ib, tx, state, NULL, 0, header, meta);
        ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);
    }
    return rc;
}

/**
 * Run stream TXDATA rules
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] data Transaction data.
 * @param[in] data_length Length of @a data.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_txdata_rules(ib_engine_t *ib,
                                           ib_tx_t *tx,
                                           ib_state_t state,
                                           const char *data,
                                           size_t data_length,
                                           void *cbdata)
{
    assert(ib != NULL);
    assert(cbdata != NULL);

    if (data == NULL) {
        return IB_OK;
    }
    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *)cbdata;
    ib_status_t rc;

    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);
    rc = run_stream_rules(ib, tx, state, data, data_length, NULL, meta);
    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);
    return rc;
}

/**
 * Run stream TX rules (aka request header)
 *
 * @param[in] ib Engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] cbdata Callback data (actually phase_rule_cbdata_t)
 *
 * @returns Status code
 */
static ib_status_t run_stream_tx_rules(ib_engine_t *ib,
                                       ib_tx_t *tx,
                                       ib_state_t state,
                                       void *cbdata)
{
    ib_parsed_headers_t *hdrs;
    ib_status_t rc;

    assert(ib != NULL);
    assert(tx != NULL);
    assert(cbdata != NULL);

    const ib_rule_phase_meta_t *meta = (const ib_rule_phase_meta_t *) cbdata;

    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);

    /* Wrap up the request line */
    rc = ib_parsed_headers_create(&hdrs, tx->mm);
    if (rc != IB_OK) {
        ib_log_error_tx(tx, "Error creating name/value pair list: %s",
                        ib_status_to_string(rc));
        return rc;
    }
    if ( (tx->request_line != NULL) &&
         (tx->request_line->method != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->method) != NULL) )
    {
        rc = ib_parsed_headers_add(
            hdrs,
            "method", 6,
            (const char *)ib_bytestr_const_ptr(tx->request_line->method),
            ib_bytestr_length(tx->request_line->method));
        if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Error adding method to "
                                 "name/value pair list: %s",
                                 ib_status_to_string(rc));
            return rc;
        }
    }

    if ( (tx->request_line != NULL) &&
         (tx->request_line->uri != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->uri) != NULL) )
    {
        rc = ib_parsed_headers_add(
            hdrs,
            "uri", 3,
            (const char *)ib_bytestr_const_ptr(tx->request_line->uri),
            ib_bytestr_length(tx->request_line->uri));
        if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Error adding uri to name/value pair list: %s",
                                 ib_status_to_string(rc));
            return rc;
        }
    }

    if ( (tx->request_line != NULL) &&
         (tx->request_line->protocol != NULL) &&
         (ib_bytestr_const_ptr(tx->request_line->protocol) != NULL) )
    {
        rc = ib_parsed_headers_add(
            hdrs,
            "protocol", 8,
            (const char *)ib_bytestr_const_ptr(tx->request_line->protocol),
            ib_bytestr_length(tx->request_line->protocol));
        if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Error adding protocol to name/value "
                                 "pair list: %s",
                                 ib_status_to_string(rc));
            return rc;
        }
    }

    /* Now, process the request line */
    if ( (hdrs != NULL) && (hdrs->head != NULL) ) {
        ib_rule_log_tx_trace(tx, "Running header line through stream header");
        rc = run_stream_rules(ib, tx, state, NULL, 0, hdrs->head, meta);
        if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Error processing tx request line: %s",
                                 ib_status_to_string(rc));
            return rc;
        }
    }

    /* Process the request header */
    if ( (tx->request_header != NULL) && (tx->request_header->head != NULL) ) {
        ib_rule_log_tx_trace(tx, "Running header through stream header");
        rc = run_stream_rules(
            ib, tx, state, NULL, 0, tx->request_header->head, meta);
        if (rc != IB_OK) {
            ib_rule_log_tx_error(tx,
                                 "Error processing tx request line: %s",
                                 ib_status_to_string(rc));
            return rc;
        }
    }

    ib_flags_clear(tx->flags, IB_TX_FALLOW_PHASE);

    return IB_OK;
}

/**
 * Initialize rule set objects.
 *
 * @param[in] ib Engine
 * @param[in] mm Memory manager to use for allocations
 * @param[in,out] ctx_rules Context's rules
 *
 * @returns Status code
 */
static ib_status_t init_ruleset(ib_engine_t *ib,
                                ib_mm_t mm,
                                ib_rule_context_t *ctx_rules)
{
    ib_status_t rc;
    ib_rule_phase_num_t    phase_num;

    /* Initialize the phase rules */
    for (phase_num = IB_PHASE_NONE;
         phase_num < IB_RULE_PHASE_COUNT;
         ++phase_num)
    {
        ib_ruleset_phase_t *ruleset_phase =
            &(ctx_rules->ruleset.phases[phase_num]);
        ruleset_phase->phase_num = (ib_rule_phase_num_t)phase_num;
        rc = find_phase_meta(phase_num, &(ruleset_phase->phase_meta));
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error finding phase meta data: %s",
                         ib_status_to_string(rc));
            return rc;
        }

        rc = ib_list_create(&(ruleset_phase->rule_list), mm);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error creating phase ruleset list: %s",
                         ib_status_to_string(rc));
            return rc;
        }
    }

    /* Create a hash to hold rules indexed by ID */
    rc = ib_hash_create_nocase(&(ctx_rules->rule_hash), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating ruleset hash: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/**
 * Register rules callbacks
 *
 * @param[in] ib Engine
 * @param[in] mm Memory manager to use for allocations
 * @param[in,out] rule_engine Rule engine
 *
 * @returns Status code
 */
static ib_status_t register_callbacks(ib_engine_t *ib,
                                      ib_mm_t mm,
                                      ib_rule_engine_t *rule_engine)
{
    const ib_rule_phase_meta_t *meta;
    const char                 *hook_type = NULL;
    ib_status_t                 rc = IB_OK;



    /* Register specific handlers for specific states, and a
     * generic handler for the rest */
    for (meta = rule_phase_meta; meta->phase_num != IB_PHASE_INVALID; ++meta) {
        if (meta->state == (ib_state_t) -1) {
            continue;
        }

        /* Phase rules (non-stream rules) all use the same callback */
        if (! meta->is_stream) {
            rc = ib_hook_tx_register(
                ib,
                meta->state,
                run_phase_rules,
                (void *)meta);
            hook_type = "tx";
        }
        else {
            switch (meta->hook_type) {

            case IB_STATE_HOOK_TX:
                rc = ib_hook_tx_register(
                    ib,
                    meta->state,
                    run_stream_tx_rules,
                    (void *)meta);
                hook_type = "stream-tx";
                break;

            case IB_STATE_HOOK_TXDATA:
                rc = ib_hook_txdata_register(
                    ib,
                    meta->state,
                    run_stream_txdata_rules,
                    (void *)meta);
                hook_type = "txdata";
                break;

            case IB_STATE_HOOK_HEADER:
                rc = ib_hook_parsed_header_data_register(
                    ib,
                    meta->state,
                    run_stream_header_rules,
                    (void *)meta);
                hook_type = "header";
                break;

            default:
                ib_log_error(ib,
                             "Unknown hook registration type %d for "
                             "phase %d/\"%s\"",
                             meta->phase_num, meta->state,
                             phase_description(meta));
                return IB_EINVAL;
            }
        }

        /* OK */
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error registering hook \"%s\" for phase "
                         "%d/%d/\"%s\": %s",
                         hook_type, meta->phase_num, meta->state,
                         phase_description(meta),
                         ib_status_to_string(rc));
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Initialize a rule engine object.
 *
 * @param[in] ib Engine
 * @param[in] mm Memory manager to use for allocations
 * @param[out] p_rule_engine Pointer to new rule engine object
 *
 * @returns Status code
 */
static ib_status_t create_rule_engine(const ib_engine_t *ib,
                                      ib_mm_t mm,
                                      ib_rule_engine_t **p_rule_engine)
{
    ib_rule_engine_t    *rule_engine;
    ib_status_t          rc;
    ib_rule_phase_num_t  phase;

    /* Create the rule object */
    rule_engine =
        (ib_rule_engine_t *)ib_mm_calloc(mm, 1, sizeof(*rule_engine));
    if (rule_engine == NULL) {
        return IB_EALLOC;
    }

    /* Indices start at 0 */
    rule_engine->index_limit = 0;

    /* Create the rule list */
    rc = ib_list_create(&(rule_engine->rule_list), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating rule engine rule list: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the main rule hash, used to index rules by ID */
    rc = ib_hash_create_nocase(&(rule_engine->rule_hash), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating rule engine rule hash: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the external drivers hash */
    rc = ib_hash_create(&(rule_engine->external_drivers), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating rule engine external rules hash: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the ownership cb list */
    rc = ib_list_create(&(rule_engine->ownership_cbs), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating rule engine ownership callback list: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the injection cb lists */
    for (phase = IB_PHASE_NONE; phase < IB_RULE_PHASE_COUNT; ++phase) {
        rc = ib_list_create(&(rule_engine->injection_cbs[phase]), mm);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error creating rule engine injection callback list: %s",
                         ib_status_to_string(rc));
            return rc;
        }
    }

    /* Setup hook lists */
    rc = ib_list_create(&(rule_engine->hooks.pre_rule), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine pre rule hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }
    rc = ib_list_create(&(rule_engine->hooks.post_rule), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine post rule hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }
    rc = ib_list_create(&(rule_engine->hooks.pre_operator), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine pre operator hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }
    rc = ib_list_create(&(rule_engine->hooks.post_operator), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine post operator hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }
    rc = ib_list_create(&(rule_engine->hooks.pre_action), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine pre action hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }
    rc = ib_list_create(&(rule_engine->hooks.post_action), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error creating rule engine post action hook callback list: %s",
            ib_status_to_string(rc)
        );
        return rc;
    }

    *p_rule_engine = rule_engine;
    return IB_OK;
}

/**
 * Initialize a context's rule object.
 *
 * @param[in] ib Engine
 * @param[in] mm Memory manager to use for allocations
 * @param[out] p_ctx_rules Pointer to new rule context object
 *
 * @returns Status code
 */
static ib_status_t create_rule_context(const ib_engine_t *ib,
                                       ib_mm_t mm,
                                       ib_rule_context_t **p_ctx_rules)
{
    assert(ib != NULL);
    assert(p_ctx_rules != NULL);

    ib_rule_context_t *ctx_rules;
    ib_status_t        rc;

    /* Create the rule object */
    ctx_rules =
        (ib_rule_context_t *)ib_mm_calloc(mm, 1, sizeof(*ctx_rules));
    if (ctx_rules == NULL) {
        return IB_EALLOC;
    }

    /* Create the rule list */
    rc = ib_list_create(&(ctx_rules->rule_list), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error initializing rule engine rule list: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the rule enable/disable lists */
    rc = ib_list_create(&(ctx_rules->enable_list), mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error initializing rule engine rule enable list: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    *p_ctx_rules = ctx_rules;
    return IB_OK;
}

/**
 * Copy a list of rules from one list to another
 *
 * @param[in] src_list List of items to copy
 * @param[in,out] dest_list List to copy items into
 *
 * @returns Status code
 */
static ib_status_t copy_rule_list(const ib_list_t *src_list,
                                  ib_list_t *dest_list)
{
    assert(src_list != NULL);
    assert(dest_list != NULL);

    return ib_list_copy_nodes(src_list, dest_list);
}

/**
 * Copy all rules from @a src_hash to @a dest_hash.
 *
 * @param[in] ctx Provides access to the IronBee engine temp
 *                mpool and logging.
 * @param[in] src_hash Hash of rules to copy
 * @param[in,out] dest_hash Hash to copy rules into
 *
 * @returns
 *   - IB_OK on success, including if the @a src_hash is size 0.
 *   - IB_EALLOC if a temporary list cannot be made.
 */
static ib_status_t copy_rule_hash(const ib_context_t *ctx,
                                  const ib_hash_t *src_hash,
                                  ib_hash_t *dest_hash)
{
    assert(src_hash != NULL);
    assert(dest_hash != NULL);
    ib_status_t rc;
    ib_hash_iterator_t *iterator;

    if (ib_hash_size(src_hash) == 0) {
        return IB_OK;
    }
    iterator = ib_hash_iterator_create(ib_engine_mm_temp_get(ctx->ib));
    if (iterator == NULL) {
        return IB_EALLOC;
    }
    ib_hash_iterator_first(iterator, src_hash);

    while (! ib_hash_iterator_at_end(iterator)) {
        const ib_rule_t *rule;
        ib_hash_iterator_fetch(NULL, NULL, &rule, iterator);

        assert(rule != NULL);

        rc = ib_hash_set(dest_hash, rule->meta.id, (void *)rule);
        if (rc != IB_OK) {
            return rc;
        }

        ib_hash_iterator_next(iterator);
    }
    return IB_OK;
}

/**
 * Import a rule's context from it's parent
 *
 * @param[in] ctx Context being imported to
 * @param[in] parent_rules Parent's rule context object
 * @param[in,out] ctx_rules Rule context object
 *
 * @returns Status code
 */
static ib_status_t import_rule_context(const ib_context_t *ctx,
                                       const ib_rule_context_t *parent_rules,
                                       ib_rule_context_t *ctx_rules)
{
    assert(parent_rules != NULL);
    assert(ctx_rules != NULL);
    ib_status_t rc;

    /* Copy rules list */
    rc = copy_rule_list(parent_rules->rule_list, ctx_rules->rule_list);
    if (rc != IB_OK) {
        return rc;
    }

    /* Copy enable list */
    rc = copy_rule_list(parent_rules->enable_list, ctx_rules->enable_list);
    if (rc != IB_OK) {
        return rc;
    }

    /* Copy rule hash */
    rc = copy_rule_hash(ctx, parent_rules->rule_hash, ctx_rules->rule_hash);
    if (rc != IB_OK) {
        return rc;
    }
    return IB_OK;
}


/**
 * Enable/disable an individual rule
 *
 * @param[in] enable true:Enable, false:Disable
 * @param[in,out] ctx_rule Context rule to enable/disable
 */
static void set_rule_enable(bool enable,
                            ib_rule_ctx_data_t *ctx_rule)
{
    assert(ctx_rule != NULL);

    if (enable) {
        ib_flags_set(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
    }
    else {
        ib_flags_clear(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
    }
    return;
}

/* Rule enable functions. */
static ib_status_t rule_enable_all(const ib_rule_t *rule, void *cbdata) {
    assert(rule != NULL);
    return IB_OK;
}

static ib_status_t rule_enable_by_tag(const ib_rule_t *rule, void *cbdata) {
    assert(rule != NULL);
    assert(cbdata != NULL);

    if (ib_rule_tag_match(rule, (const char *)cbdata)) {
        return IB_OK;
    }

    return IB_DECLINED;
}

static ib_status_t rule_enable_by_id(const ib_rule_t *rule, void *cbdata) {
    assert(rule != NULL);
    assert(cbdata != NULL);

    if (ib_rule_id_match(rule, (const char *)cbdata)) {
        return IB_OK;
    }

    return IB_DECLINED;
}

static ib_status_t rule_enable_by_id_prefix(
    const ib_rule_t *rule,
    void            *cbdata
)
{
    assert(rule != NULL);
    assert(cbdata != NULL);

    const char *id     = (const char *)cbdata;
    size_t      id_len = strlen(id);

    /* First match the rule's ID and full ID */
    if ( (strncasecmp(id, rule->meta.id, id_len) == 0) ||
         (strncasecmp(id, rule->meta.full_id, id_len) == 0) )
    {
        return IB_OK;
    }

    /* Check parent rules. */
    for (
        ib_rule_t *parent = rule->chained_from;
        parent != NULL;
        parent = parent->chained_from
    )
    {
        if ( (strncasecmp(id, rule->meta.id, id_len) == 0) ||
             (strncasecmp(id, rule->meta.full_id, id_len) == 0) )
        {
            return IB_OK;
        }
    }

    /* Check child rules. */
    for (
        ib_rule_t *child = rule->chained_rule;
        child != NULL;
        child = child->chained_rule
    )
    {
        if ( (strncasecmp(id, rule->meta.id, id_len) == 0) ||
             (strncasecmp(id, rule->meta.full_id, id_len) == 0) )
        {
            return IB_OK;
        }
    }

    /* Finally, no match */
    return IB_DECLINED;
}

static ib_status_t rule_enable_by_tag_prefix(
    const ib_rule_t *rule,
    void            *cbdata
)
{
    assert(rule != NULL);
    assert(cbdata != NULL);

    const char *tag = (const char *)cbdata;
    size_t      tag_len = strlen(tag);

    const ib_list_node_t *node;

    /* First match the rule's tags */
    IB_LIST_LOOP_CONST(rule->meta.tags, node) {
        const char *ruletag = (const char *)ib_list_node_data_const(node);
        if (strncasecmp(tag, ruletag, tag_len) == 0) {
            return IB_OK;
        }
    }

    /* Check parent rules. */
    for (
        ib_rule_t *parent = rule->chained_from;
        parent != NULL;
        parent = parent->chained_from
    )
    {
        IB_LIST_LOOP_CONST(parent->meta.tags, node) {
            const char *ruletag = (const char *)ib_list_node_data_const(node);
            if (strncasecmp(tag, ruletag, tag_len) == 0) {
                return IB_OK;
            }
        }
    }

    /* Check child rules. */
    for (
        ib_rule_t *child = rule->chained_rule;
        child != NULL;
        child = child->chained_rule
    )
    {
        IB_LIST_LOOP_CONST(child->meta.tags, node) {
            const char *ruletag = (const char *)ib_list_node_data_const(node);
            if (strncasecmp(tag, ruletag, tag_len) == 0) {
                return IB_OK;
            }
        }
    }

    /* Finally, no match */
    return IB_DECLINED;
}



/* End Rule enable functions. */

/**
 * Enable rules that match tag / id
 *
 * @param[in] ib IronBee engine
 * @param[in] ctx Current IronBee context
 * @param[in] match Enable match data
 * @param[in,out] ctx_rule_list List of rules to search for matches to @a enable
 *
 * @returns Status code
 */
static ib_status_t enable_rules(
    ib_engine_t            *ib,
    ib_context_t           *ctx,
    const ib_rule_enable_t *match,
    ib_list_t              *ctx_rule_list
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(match != NULL);
    assert(ctx_rule_list != NULL);

    ib_list_node_t *node;
    unsigned int    matches = 0;
    const char     *lcname = match->enable ? "enable" : "disable";

    IB_LIST_LOOP(ctx_rule_list, node) {
        ib_rule_ctx_data_t *ctx_rule;
        ctx_rule = (ib_rule_ctx_data_t *)ib_list_node_data(node);
        ib_status_t rc;
        rc = match->rule_enable_fn(ctx_rule->rule, match->rule_enable_cbdata);

        if (rc == IB_OK) {
            matches++;
            set_rule_enable(match->enable, ctx_rule);
        }
    }

    if (match->require_match && matches == 0) {

        ib_cfg_log_notice_ex(
            ib,
            match->file,
            match->lineno,
            "Failed to %s any rules with %s.",
            lcname,
            match->name
        );
        return IB_ENOENT;
    }
    else {
        ib_cfg_log_debug_ex(
            ib,
            match->file,
            match->lineno,
            "Enabled %u rules by %s.",
            matches,
            match->name
        );
    }

    return IB_OK;
}

bool ib_rule_is_chained(const ib_rule_t *rule) {
    return ib_flags_any(rule->flags, IB_RULE_FLAG_CHCHILD);
}

bool ib_rule_is_marked(const ib_rule_t *rule) {
    return ib_flags_any(rule->flags, IB_RULE_FLAG_MARK);
}

/**
 * Close a context for the rule engine.
 *
 * Called when a context is closed; performs rule engine rule fixups.
 *
 *
 * @param[in,out] ib IronBee object
 * @param[in,out] ctx IronBee context
 * @param[in] state State
 * @param[in] cbdata Callback data (unused)
 */
static ib_status_t rule_engine_ctx_close(ib_engine_t *ib,
                                         ib_context_t *ctx,
                                         ib_state_t state,
                                         void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(state == context_close_state);
    assert(cbdata == NULL);

    ib_list_t      *all_rules;
    ib_list_node_t *node;
    ib_context_t   *main_ctx = ib_context_main(ib);
    ib_status_t     rc;

    /* Don't enable rules for non-location contexts */
    if (ctx->ctype != IB_CTYPE_LOCATION) {
        return IB_OK;
    }

    /* Create the list of all rules */
    rc = ib_list_create(&all_rules, ctx->mm);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error initializing rule engine rule list: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Step 1: Unmark all rules in the context's rule list */
    IB_LIST_LOOP(ctx->rules->rule_list, node) {
        ib_rule_t    *rule = (ib_rule_t *)ib_list_node_data(node);
        ib_flags_clear(rule->flags, IB_RULE_FLAG_MARK);
    }

    /* Step 2: Loop through all of the rules in the main context, add them
     * to the list of all rules */
    IB_LIST_LOOP(main_ctx->rules->rule_list, node) {
        ib_rule_t          *ref = (ib_rule_t *)ib_list_node_data(node);
        ib_rule_t          *rule = NULL;
        ib_rule_ctx_data_t *ctx_rule = NULL;

        /* If it's a chained rule, skip it */
        if (ib_rule_is_chained(ref)) {
            continue;
        }

        /* Find the appropriate version of the rule to use */
        rc = ib_rule_lookup(ib, ctx, ref->meta.id, &rule);
        if (rc != IB_OK) {
            ib_log_error(ib, "Error looking up rule \"%s\": %s",
                         ref->meta.id, ib_status_to_string(rc));
            return rc;
        }

        /* Create a rule ctx object for it, store it in the list */
        ctx_rule = ib_mm_alloc(ctx->mm, sizeof(*ctx_rule));
        if (ctx_rule == NULL) {
            return IB_EALLOC;
        }
        ctx_rule->rule = rule;
        if (! ib_flags_all(rule->flags, IB_RULE_FLAG_MAIN_CTX)) {
            ctx_rule->flags = IB_RULECTX_FLAG_ENABLED;
            ib_flags_set(rule->flags, IB_RULE_FLAG_MARK);
        }
        else {
            ctx_rule->flags = IB_RULECTX_FLAG_NONE;
        }
        rc = ib_list_push(all_rules, ctx_rule);
        if (rc != IB_OK) {
            return IB_EALLOC;
        }
    }

    /* Step 3: Loop through all of the context's rules, add them
     * to the list of all rules if they're not marked... */
    IB_LIST_LOOP(ctx->rules->rule_list, node) {
        ib_rule_t          *rule = (ib_rule_t *)ib_list_node_data(node);
        ib_rule_ctx_data_t *ctx_rule;

        /* If the rule is chained or marked */
        if (ib_rule_is_chained(rule) || ib_rule_is_marked(rule)) {
            continue;
        }

        /* Create a ctx object for it, store it in the list */
        ctx_rule = ib_mm_alloc(ctx->mm, sizeof(*ctx_rule));
        if (ctx_rule == NULL) {
            return IB_EALLOC;
        }
        ctx_rule->rule = rule;
        ib_flags_set(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED);
        rc = ib_list_push(all_rules, ctx_rule);
        if (rc != IB_OK) {
            return IB_EALLOC;
        }
    }

    /* Step 4: Enable / Disable rules. */
    IB_LIST_LOOP(ctx->rules->enable_list, node) {
        const ib_rule_enable_t *enable;
        enable = (const ib_rule_enable_t *)ib_list_node_data(node);

        rc = enable_rules(ib, ctx, enable, all_rules);
        if (rc != IB_OK) {
            ib_cfg_log_notice_ex(ib, enable->file, enable->lineno,
                                 "Error apply rule enable/disable "
                                 "in \"%s\" temp list",
                                 ib_context_full_get(ctx));
        }
    }

    /* Step 5: Add all enabled rules to the appropriate execution list */
    IB_LIST_LOOP(all_rules, node) {
        ib_rule_ctx_data_t   *ctx_rule;
        ib_ruleset_phase_t   *ruleset_phase;
        ib_list_t            *phase_rule_list;
        ib_rule_phase_num_t   phase_num;
        ib_rule_t            *rule;
        const ib_list_node_t *onode;
        size_t                owners = 0;
        const char           *owner_name = NULL;

        ctx_rule = (ib_rule_ctx_data_t *)ib_list_node_data(node);
        assert(ctx_rule != NULL);
        rule = ctx_rule->rule;

        /* If it's not enabled, skip to the next rule */
        if (! ib_flags_all(ctx_rule->flags, IB_RULECTX_FLAG_ENABLED)) {
            continue;
        }

        phase_num = rule->meta.phase;

        /* Give the ownership functions a shot at the rule */
        IB_LIST_LOOP_CONST(ib->rule_engine->ownership_cbs, onode) {
            const ib_rule_ownership_cb_t *cb =
                (const ib_rule_ownership_cb_t *)ib_list_node_data_const(onode);
            ib_status_t orc;

            orc = cb->fn(ib, rule, ctx, cb->data);
            if (orc == IB_OK) {
                ib_log_debug2(ib,
                              "Ownership callback \"%s\" has taken ownership "
                              "of rule \"%s\" phase=%d context=\"%s\"",
                              cb->name,
                              ib_rule_id(rule), phase_num,
                              ib_context_full_get(ctx));
                ++owners;
                owner_name = cb->name;

                /* Report multiple owners as an error. */
                if (owners > 1) {
                    ib_log_error(
                        ib,
                        "Rule owned by \"%s\" was also claimed by \"%s\" "
                        "in rule \"%s\" phase=%d context=\"%s\".",
                        owner_name,
                        cb->name,
                        ib_rule_id(rule),
                        phase_num,
                        ib_context_full_get(ctx));
                }
                break;
            }
            /* Ownership may only return IB_OK or IB_DECLINED. */
            else if (orc != IB_DECLINED) {
                ib_log_error(ib,
                             "Ownership callback \"%s\" returned an error "
                             "for rule \"%s\" phase=%d context=\"%s\": %s",
                             cb->name,
                             ib_rule_id(rule), phase_num,
                             ib_context_full_get(ctx),
                             ib_status_to_string(orc));
                return IB_EUNKNOWN;
            }
        }
        if (owners > 0) {
            continue;
        }

        /* Rules in default engine need phases. */
        /* Sanity checks */
        if( (rule->phase_meta->flags & PHASE_FLAG_IS_VALID) == 0) {
            ib_log_error(ib, "Cannot register rule: Phase is invalid");
            return IB_EINVAL;
        }
        if (! is_phase_num_valid(phase_num) || phase_num == IB_PHASE_NONE) {
            ib_log_error(ib, "Cannot register rule: Invalid phase %d", phase_num);
            return IB_EINVAL;
        }
        assert (rule->meta.phase == rule->phase_meta->phase_num);

        /* Determine what phase list to add it into */
        ruleset_phase = &(ctx->rules->ruleset.phases[phase_num]);
        assert(ruleset_phase != NULL);
        assert(ruleset_phase->phase_meta == rule->phase_meta);
        phase_rule_list = ruleset_phase->rule_list;

        /* Add it to the list */
        rc = ib_list_push(phase_rule_list, (void *)ctx_rule);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error adding rule type=\"%s\" phase=%d "
                         "context=\"%s\": %s",
                         rule->phase_meta->is_stream ? "Stream" : "Normal",
                         phase_num,
                         ib_context_full_get(ctx),
                         ib_status_to_string(rc));
            return rc;
        }

        ib_log_debug(ib,
                     "Enabled rule \"%s\" rev=%u type=\"%s\" phase=%d/\"%s\" "
                     "for context \"%s\"",
                     ib_rule_id(rule), rule->meta.revision,
                     rule->phase_meta->is_stream ? "Stream" : "Normal",
                     phase_num, phase_name(rule->phase_meta),
                     ib_context_full_get(ctx));
    }

    /* Initialize var sources */
    {
        ib_rule_engine_t *re = ib->rule_engine;
        const ib_var_config_t *config =  ib_engine_var_config_get(ib);
        ib_mm_t mm = ib_engine_mm_main_get(ib);

/* Helper Macro */
#define RE_SOURCE(name, src) \
    { \
        ib_var_source_t *temp; \
        rc = ib_var_source_acquire( \
            &temp, mm, config, IB_S2SL((name)) \
        ); \
        if (rc != IB_OK) { \
            ib_log_error(ib, \
                "Error acquiring var source: %s: %s", \
                (name), ib_status_to_string(rc) \
            ); \
            return rc; \
        } \
        re->source.src = temp; \
    }
/* End Helper Macro */

        RE_SOURCE("FIELD",           field);
        RE_SOURCE("FIELD_TARGET",    field_target);
        RE_SOURCE("FIELD_TFN",       field_tfn);
        RE_SOURCE("FIELD_NAME",      field_name);
        RE_SOURCE("FIELD_NAME_FULL", field_name_full);

#undef RE_SOURCE
    }

    ib_rule_log_flags_dump(ib, ctx);

    return IB_OK;
}

/**
 * Rule engine context open
 *
 * Called when a context is opened; performs rule engine context-specific
 * initializations.
 *
 * @param[in] ib IronBee object
 * @param[in] ctx IronBee context
 * @param[in] state State
 * @param[in] cbdata Callback data (unused)
 */
static ib_status_t rule_engine_ctx_open(ib_engine_t *ib,
                                        ib_context_t *ctx,
                                        ib_state_t state,
                                        void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(state == context_open_state);
    assert(cbdata == NULL);

    ib_status_t rc;

    /* Late registration of the context close state */
    if (ib_context_type(ctx) == IB_CTYPE_MAIN) {
        ib_var_config_t *config;

        rc = ib_hook_context_register(ib, context_close_state,
                                      rule_engine_ctx_close, NULL);
        if (rc != IB_OK) {
            return rc;
        }

        /* Register indexed vars.  Do this here instead of at init to avoid
         * requiring that the data subsystem be initialized before the
         * rule subsystem.
         */
        config = ib_engine_var_config_get(ib);
        assert(config != NULL);
        for (
            const char **key = indexed_keys;
            *key != NULL;
            ++key
        )
        {
            rc = ib_var_source_register(
                NULL,
                config,
                IB_S2SL(*key),
                IB_PHASE_NONE, IB_PHASE_NONE
            );
            if (rc != IB_OK) {
                ib_log_warning(ib,
                    "Error registering \"%s\" as indexed var: %s",
                    *key,
                    ib_status_to_string(rc)
                );
            }
            /* Do not abort.  Everything should still work, just be a little
             * slower.
             */
        }
    }

    /* If the rules are already initialized for this context, do nothing */
    if (ctx->rules != NULL) {
        return IB_OK;
    }

    /* Create the rule engine object */
    rc = create_rule_context(ib, ctx->mm, &(ctx->rules));
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error initializing rule engine context rules: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Initialize the rule sets */
    rc = init_ruleset(ib, ctx->mm, ctx->rules);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error initializing rule engine phase ruleset: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* If this is a location context, import our parents context info */
    if (ctx->ctype == IB_CTYPE_LOCATION) {
        rc = import_rule_context(ctx, ctx->parent->rules, ctx->rules);
        if (rc != IB_OK) {
            ib_log_error(ib,
                         "Error importing rule engine context from parent: %s",
                         ib_status_to_string(rc));
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Handle the transaction starting.
 *
 * @param ib Engine.
 * @param tx Transaction.
 * @param state State.
 * @param cbdata Callback data.
 *
 * @returns Status code.
 */
static ib_status_t rule_engine_tx_started(ib_engine_t *ib,
                                          ib_tx_t *tx,
                                          ib_state_t state,
                                          void *cbdata)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(state == tx_started_state);
    assert(cbdata == NULL);

    ib_status_t rc;
    ib_rule_exec_t *rule_exec;

    /* Don't allow the user to create a second rule exec object */
    if (tx->rule_exec != NULL) {
        return IB_EINVAL;
    }
    /* Create the rule engine execution environment object */
    rc = rule_exec_create(tx, &rule_exec);
    if (rc != IB_OK) {
        ib_rule_log_tx_error(tx,
                             "Failed to create rule execution object: %s",
                             ib_status_to_string(rc));
        return rc;
    }
    tx->rule_exec = rule_exec;

    return IB_OK;
}

ib_status_t ib_rule_engine_init(ib_engine_t *ib)
{
    ib_status_t rc;

    /* Create the rule engine object */
    rc = create_rule_engine(
        ib,
        ib_engine_mm_main_get(ib),
        &(ib->rule_engine)
    );
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating rule engine: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the tx start event */
    rc = ib_hook_tx_register(ib, tx_started_state,
                             rule_engine_tx_started, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the rule callbacks */
    rc = register_callbacks(ib, ib_engine_mm_main_get(ib), ib->rule_engine);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error registering rule engine phase callbacks: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Register the context open callback -- it'll register the
     * context close handler at the open of the main context. */
    rc = ib_hook_context_register(ib, context_open_state,
                                  rule_engine_ctx_open, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_mm_t ib_rule_mm(ib_engine_t *ib)
{
    return ib_engine_mm_config_get(ib);
}


/**
 * Calculate a rule's position in a chain.
 *
 * @param[in] ib IronBee Engine
 * @param[in] rule The rule
 * @param[out] pos The rule's position in the chain
 *
 * @returns Status code
 */
static ib_status_t chain_position(ib_engine_t *ib,
                                  const ib_rule_t *rule,
                                  ib_num_t *pos)
{
    ib_num_t n = 0;

    /* Loop through all parent rules */
    while (rule != NULL) {
        ++n;
        rule = rule->chained_from;
    }; /* while (rule != NULL); */
    *pos = n;
    return IB_OK;
}


/**
 * Generate the id for a chained rule.
 *
 * @param[in] ib IronBee Engine
 * @param[in,out] rule The rule
 * @param[in] force Set the ID even if it's already set
 *
 * @returns Status code
 */
static ib_status_t chain_gen_rule_id(ib_engine_t *ib,
                                     ib_rule_t *rule,
                                     bool force)
{
    ib_status_t rc;
    char idbuf[128];
    ib_num_t pos;

    /* If it's already set, do nothing */
    if ( (rule->meta.id != NULL) && (! force) ) {
        return IB_OK;
    }

    rc = chain_position(ib, rule, &pos);
    if (rc != IB_OK) {
        return rc;
    }
    snprintf(idbuf, sizeof(idbuf), "%s/%d", rule->meta.chain_id, (int)pos);
    rule->meta.id = ib_mm_strdup(ib_rule_mm(ib), idbuf);
    if (rule->meta.id == NULL) {
        return IB_EALLOC;
    }
    return IB_OK;
}

/**
 * Make @a rule a chained rule of @a parent.
 *
 * This requires that @a parent have its @a ref IB_RULE_FLAG_CHPARENT flag set.
 *
 * @param[in] rule The rule to chain after @a parent.
 * @param[in] parent The rule that will precede @a rule.
 */
void rule_set_as_child(ib_rule_t *rule, ib_rule_t *parent)
{
    assert(rule != NULL);
    assert(parent != NULL);
    assert(ib_flags_any(parent->flags, IB_RULE_FLAG_CHPARENT));

    parent->chained_rule = rule;

    rule->chained_from   = parent;
    rule->meta.phase     = parent->meta.phase;
    rule->phase_meta     = parent->phase_meta;
    rule->meta.chain_id  = parent->meta.chain_id;

    ib_flags_set(rule->flags, IB_RULE_FLAG_CHCHILD);
}

ib_status_t ib_rule_create(
    ib_engine_t   *ib,
    ib_context_t  *ctx,
    const char    *file,
    unsigned int   lineno,
    bool           is_stream,
    ib_rule_t    **prule)
{
    ib_status_t                 rc;
    ib_rule_t                  *rule;
    ib_list_t                  *lst;
    ib_mm_t                     mm = ib_rule_mm(ib);
    ib_rule_context_t          *context_rules;
    ib_rule_t                  *previous;
    const ib_rule_phase_meta_t *phase_meta;

    assert(ib != NULL);
    assert(ctx != NULL);

    /* Look up the generic rule phase */
    rc = find_meta(is_stream, IB_PHASE_NONE, &phase_meta);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error looking up rule phase: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Allocate the rule */
    rule = (ib_rule_t *)ib_mm_calloc(mm, sizeof(ib_rule_t), 1);
    if (rule == NULL) {
        return IB_EALLOC;
    }
    rule->flags            = is_stream ?
        IB_RULE_FLAG_STREAM : IB_RULE_FLAG_NONE;
    rule->phase_meta       = phase_meta;
    rule->meta.phase       = IB_PHASE_NONE;
    rule->meta.revision    = 1;
    rule->meta.config_file = ib_mm_strdup(mm, file);
    rule->meta.config_line = lineno;
    rule->meta.index       = ib->rule_engine->index_limit;
    rule->ctx              = ctx;
    rule->opinst           = NULL;
    ++ib->rule_engine->index_limit;

    /* Note if this is the main context */
    if (ctx == ib_context_main(ib)) {
        rule->flags |= IB_RULE_FLAG_MAIN_CTX;
    }

    /* Meta tags list */
    lst = NULL;
    rc = ib_list_create(&lst, mm);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating rule meta tags list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    rule->meta.tags = lst;

    /* Target list */
    lst = NULL;
    rc = ib_list_create(&lst, mm);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating rule target field list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    rule->target_fields = lst;

    /* Create the True Action list */
    lst = NULL;
    rc = ib_list_create(&lst, mm);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating rule true action list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    rule->true_actions = lst;

    /* Create the False Action list */
    lst = NULL;
    rc = ib_list_create(&lst, mm);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating rule false action list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    rule->false_actions = lst;

    /* Create the Auxiliary Action list. */
    lst = NULL;
    rc = ib_list_create(&lst, mm);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error creating rule aux action list: %s",
                     ib_status_to_string(rc));
        return rc;
    }
    rule->aux_actions = lst;


    /* Get the rule engine and previous rule */
    context_rules = ctx->rules;
    previous = context_rules->parser_data.previous;

    /*
     * If the previous rule has it's CHAIN flag set,
     * chain to that rule & update the current rule.
     */
    if (  (previous != NULL) &&
          ((previous->flags & IB_RULE_FLAG_CHPARENT) != 0) )
    {
        rule_set_as_child(rule, previous);
    }

    /* Good */
    rule->parent_rlist = ctx->rules->rule_list;
    *prule = rule;
    return IB_OK;
}

ib_flags_t ib_rule_required_op_flags(const ib_rule_t *rule)
{
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    return rule->phase_meta->required_op_flags;
}

bool ib_rule_allow_tfns(const ib_rule_t *rule)
{
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if (ib_flags_all(rule->flags, IB_RULE_FLAG_NO_TGT)) {
        return false;
    }
    else if (ib_flags_all(rule->phase_meta->flags, PHASE_FLAG_ALLOW_TFNS)) {
        return true;
    }
    else {
        return false;
    }
}

bool ib_rule_allow_chain(const ib_rule_t *rule)
{
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->phase_meta->flags & PHASE_FLAG_ALLOW_CHAIN) != 0) {
        return true;
    }
    else {
        return false;
    }
}

bool ib_rule_is_stream(const ib_rule_t *rule)
{
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->phase_meta->flags & PHASE_FLAG_IS_STREAM) != 0) {
        return true;
    }
    else {
        return false;
    }
}

ib_status_t ib_rule_set_chain(ib_engine_t *ib,
                              ib_rule_t *rule)
{
    assert ((rule->phase_meta->flags & PHASE_FLAG_ALLOW_CHAIN) != 0);

    /* Set the chain flags */
    rule->flags |= IB_RULE_FLAG_CHPARENT;

    return IB_OK;
}

ib_status_t ib_rule_set_phase(ib_engine_t *ib,
                              ib_rule_t *rule,
                              ib_rule_phase_num_t phase_num)
{
    const ib_rule_phase_meta_t *phase_meta;
    ib_status_t rc;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    if ( (rule->meta.phase != IB_PHASE_NONE) &&
         (rule->meta.phase != phase_num) ) {
        ib_log_error(ib,
                     "Error setting rule phase: already set to %d",
                     rule->meta.phase);
        return IB_EINVAL;
    }
    if (! is_phase_num_valid(phase_num)) {
        ib_log_error(ib, "Error setting rule phase: Invalid phase %d",
                     phase_num);
        return IB_EINVAL;
    }

    /* Look up the real rule phase */
    rc = find_meta(rule->phase_meta->is_stream, phase_num, &phase_meta);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error looking up rule phase: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    rule->meta.phase = phase_num;
    rule->phase_meta = phase_meta;
    return IB_OK;
}

/* Lookup rule by ID */
ib_status_t ib_rule_lookup(ib_engine_t *ib,
                           ib_context_t *ctx,
                           const char *id,
                           ib_rule_t **rule)
{
    assert(ib != NULL);
    assert(id != NULL);
    assert(rule != NULL);

    ib_context_t *main_ctx = ib_context_main(ib);
    assert(main_ctx != NULL);

    ib_status_t rc;

    /* First, look in the context's rule set */
    if ( (ctx != NULL) && (ctx != main_ctx) ) {
        rc = ib_hash_get(ctx->rules->rule_hash, rule, id);
        if (rc != IB_ENOENT) {
            return rc;
        }
    }

    /* If not in the context's rule set, look in the main context */
    rc = ib_hash_get(main_ctx->rules->rule_hash, rule, id);
    return rc;
}

/* Find rule matching a reference rule */
ib_status_t ib_rule_match(ib_engine_t *ib,
                          ib_context_t *ctx,
                          const ib_rule_t *ref,
                          ib_rule_t **rule)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(ref != NULL);
    assert(rule != NULL);

    ib_status_t rc;

    /* Lookup rule with matching ID */
    rc = ib_rule_lookup(ib, ctx, ref->meta.id, rule);
    if (rc != IB_OK) {
        return IB_ENOENT;
    }

    /* Verify that phase's match */
    if (ref->meta.phase != (*rule)->meta.phase) {
        return IB_EBADVAL;
    }

    /* Done */
    return IB_OK;
}

static ib_status_t gen_full_id(ib_engine_t *ib,
                               ib_context_t *ctx,
                               ib_rule_t *rule)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(rule != NULL);
    assert(rule->meta.id != NULL);

    size_t len = 1;    /* Space for trailing nul */
    char *buf;
    const char *part1;
    const char *part2;
    const char *part3;

    /* Calculate the length */
    if (ib_flags_all(rule->flags, IB_RULE_FLAG_MAIN_CTX) ) {
        part1 = "main/";
        part2 = NULL;
        len += 5;                            /* "main/" */
    }
    else {
        ib_status_t rc;
        const ib_site_t *site;

        switch(ctx->ctype) {
        case IB_CTYPE_LOCATION :
        case IB_CTYPE_SITE :
            rc = ib_context_site_get(ctx, &site);
            if (rc != IB_OK) {
                return rc;
            }
            else if ( (site == NULL) || (site->id[0] == '\0') ) {
                ib_log_error(ib,
                             "Error creating rule ID for context rule: "
                             "no site ID");
                return IB_EINVAL;
            }
            else {
                part2 = site->id;
            }
            break;
        default:
            part2 = ctx->ctx_name;
        }
        part1 = "site/";
        len += 5 + strlen(part2) + 1; /* "site/<id>/" */
    }
    part3 = rule->meta.id;
    len += strlen(part3);

    /* Allocate the buffer */
    buf = (char *)ib_mm_alloc(ctx->mm, len);
    if (buf == NULL) {
        return IB_EALLOC;
    }

    /* Finally, build the string */
    strcpy(buf, part1);
    if (part2 != NULL) {
        strcat(buf, part2);
        strcat(buf, "/");
    }
    strcat(buf, part3);
    assert(strlen(buf) == len-1);

    rule->meta.full_id = buf;
    return IB_OK;
}

ib_status_t ib_rule_register(ib_engine_t *ib,
                             ib_context_t *ctx,
                             ib_rule_t *rule)
{
    ib_status_t          rc;
    ib_rule_context_t   *context_rules;
    ib_rule_t           *lookup;

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(rule != NULL);
    assert(rule->phase_meta != NULL);

    /* Verify that we have a valid operator */
    if (rule->opinst == NULL) {
        ib_log_error(ib, "Error registering rule: No operator instance");
        return IB_EINVAL;
    }
    if (rule->opinst->opinst == NULL) {
        ib_log_error(ib, "Error registering rule: No operator");
        return IB_EINVAL;
    }

    /* Verify that the rule has at least one target */
    if (ib_flags_any(rule->flags, IB_RULE_FLAG_NO_TGT)) {
        if (ib_list_elements(rule->target_fields) != 0) {
            ib_log_error(ib, "Error registering rule: Action rule has targets");
            return IB_EINVAL;
        }

        /* Give it a fake target */
        assert(ib_list_elements(rule->target_fields) == 0);
        ib_field_t *f = NULL;
        ib_rule_target_t *tgt = NULL;

        rc = ib_field_create(&f, ib_rule_mm(ib), IB_S2SL("NULL"),
                             IB_FTYPE_NULSTR, ib_ftype_nulstr_in("NULL"));
        if (rc != IB_OK) {
            return rc;
        }
        tgt = ib_mm_calloc(ib_rule_mm(ib), sizeof(*tgt), 1);
        if (tgt == NULL) {
            return IB_EALLOC;
        }
        tgt->target = NULL;
        tgt->target_str = "NULL";
        rc = ib_list_create(&(tgt->tfn_list), ib_rule_mm(ib));
        if (rc != IB_OK) {
            return rc;
        }
        rc = ib_list_push(rule->target_fields, tgt);
        if (rc != IB_OK) {
            return rc;
        }
    }
    else {
        if (ib_list_elements(rule->target_fields) == 0) {
            ib_log_error(ib, "error registering rule: No targets");
            return IB_EINVAL;
        }
    }

    /* Verify that the rule has an ID */
    if ( (rule->meta.id == NULL) && (rule->meta.chain_id == NULL) ) {
        ib_log_error(ib, "Error registering rule: No ID");
        return IB_EINVAL;
    }

    /* If either of the chain flags is are, the chain ID is the rule's ID */
    if (ib_flags_any(rule->flags, IB_RULE_FLAG_CHAIN)) {
        if (rule->chained_from != NULL) {
            rule->meta.chain_id = rule->chained_from->meta.chain_id;
        }
        else {
            rule->meta.chain_id = rule->meta.id;
        }
        rule->meta.id = NULL;
        rc = chain_gen_rule_id(ib, rule, true);
        if (rc != IB_OK) {
            return rc;
        }
    }

    /* Build the rule's full ID */
    rc = gen_full_id(ib, ctx, rule);
    if (rc != IB_OK) {
        return rc;
    }

    /* Get the rule engine and previous rule */
    context_rules = ctx->rules;

    /* Handle chained rule */
    if (rule->chained_from != NULL) {
        if (! ib_flags_all(rule->chained_from->flags, IB_RULE_FLAG_VALID)) {
            return IB_EINVAL;
        }
        ib_log_debug3(ib,
                      "Registered rule \"%s\" chained from rule \"%s\".",
                      ib_rule_id(rule), ib_rule_id(rule->chained_from));
    }

    /* Put this rule in the hash */
    lookup = NULL;
    rc = ib_rule_match(ib, ctx, rule, &lookup);
    switch(rc) {
    case IB_OK:
    case IB_ENOENT:
        break;
    case IB_EBADVAL:
        ib_cfg_log_error_ex(ib,
                            rule->meta.config_file,
                            rule->meta.config_line,
                            "Phase %s of rule \"%s\":%u "
                            "differs from previous definition",
                            ib_rule_phase_name(rule->meta.phase),
                            rule->meta.id, rule->meta.revision);
        ib_cfg_log_error_ex(ib,
                            lookup->meta.config_file, lookup->meta.config_line,
                            "Note: %s phase previous definition of \"%s\":%u",
                            ib_rule_phase_name(lookup->meta.phase),
                            lookup->meta.id, lookup->meta.revision);
        return rc;
    default:
        ib_cfg_log_error_ex(ib,
                            rule->meta.config_file,
                            rule->meta.config_line,
                            "Error finding matching rule "
                            "for \"%s\" of context=\"%s\": %s",
                            ib_rule_id(rule),
                            ib_context_full_get(ctx),
                            ib_status_to_string(rc));
        return rc;
    }

    /* Now, replace the existing rule if required */
    if ( (lookup != NULL) &&
         (rule->meta.revision <= lookup->meta.revision) )
    {
        ib_cfg_log_notice_ex(ib,
                             rule->meta.config_file,
                             rule->meta.config_line,
                             "Not replacing "
                             "rule \"%s\" [context:\"%s\" rev:%d] with "
                             "rule \"%s\" [context:\"%s\" rev:%d]",
                             ib_rule_id(lookup),
                             ib_context_full_get(lookup->ctx),
                             lookup->meta.revision,
                             ib_rule_id(rule),
                             ib_context_full_get(ctx),
                             rule->meta.revision);
        return IB_EEXIST;
    }

    /* Remove the old version from the hash */
    if (lookup != NULL) {
        ib_hash_remove(context_rules->rule_hash, NULL, rule->meta.id);
    }

    /* Add the new version to the hash */
    rc = ib_hash_set(context_rules->rule_hash, rule->meta.id, rule);
    if (rc != IB_OK) {
        ib_cfg_log_error_ex(ib,
                            rule->meta.config_file,
                            rule->meta.config_line,
                            "Error adding rule \"%s\" "
                            "to context=\"%s\" hash: %s",
                            ib_rule_id(rule),
                            ib_context_full_get(ctx),
                            ib_status_to_string(rc));
        return rc;
    }

    /* If no previous rule in the list, add the new rule */
    if (lookup == NULL) {

        /* Add the rule to the list */
        rc = ib_list_push(context_rules->rule_list, rule);
        if (rc != IB_OK) {
            ib_cfg_log_error_ex(ib,
                                rule->meta.config_file,
                                rule->meta.config_line,
                                "Error adding rule \"%s\" "
                                "to context=\"%s\" list: %s",
                                ib_rule_id(rule),
                                ib_context_full_get(ctx),
                                ib_status_to_string(rc));
            return rc;
        }
        ib_cfg_log_debug_ex(ib,
                            rule->meta.config_file,
                            rule->meta.config_line,
                            "Added rule \"%s\" rev=%u to context=\"%s\"",
                            ib_rule_id(rule),
                            rule->meta.revision,
                            ib_context_full_get(ctx));
    }
    else {
        /* Walk through the rule list, point at the new rule */
        ib_list_node_t    *node;
        IB_LIST_LOOP(context_rules->rule_list, node) {
            ib_rule_t *r = (ib_rule_t *)ib_list_node_data(node);
            if (strcmp(r->meta.id, rule->meta.id) == 0) {
                ib_list_node_data_set(node, rule);
            }
        }

        ib_cfg_log_debug2_ex(ib,
                             rule->meta.config_file,
                             rule->meta.config_line,
                             "Replaced "
                             "rule \"%s\" [context:\"%s\" rev:%d] with "
                             "rule \"%s\" [context:\"%s\" rev:%d]",
                             ib_rule_id(lookup),
                             ib_context_full_get(lookup->ctx),
                             lookup->meta.revision,
                             ib_rule_id(rule),
                             ib_context_full_get(ctx),
                             rule->meta.revision);
    }

    /* Mark the rule as valid */
    rule->flags |= IB_RULE_FLAG_VALID;

    /* Store off this rule for chaining */
    context_rules->parser_data.previous = rule;

    return IB_OK;
}

ib_status_t ib_rule_enable(
    const ib_engine_t   *ib,
    ib_context_t        *ctx,
    const char          *name,
    ib_rule_enable_fn_t  enable_fn,
    void                *enable_data,
    bool                 enable,
    bool                 require_match,
    const char          *file,
    unsigned int         lineno
)
{
    ib_status_t        rc;
    ib_rule_enable_t  *item;

    assert(ib != NULL);
    assert(ctx != NULL);
    assert(name != NULL);

    /* Create the enable object */
    item = ib_mm_alloc(ctx->mm, sizeof(*item));
    if (item == NULL) {
      return IB_EALLOC;
    }
    item->name = name;
    item->enable = enable;
    item->file = file;
    item->require_match = require_match;
    item->lineno = lineno;
    item->rule_enable_fn = enable_fn;
    item->rule_enable_cbdata = enable_data;

    /* Add the item to the appropriate list */
    rc = ib_list_push(ctx->rules->enable_list, item);
    if (rc != IB_OK) {
        ib_cfg_log_error_ex(ib, file, lineno,
                            "Error adding \"%s\" to context=\"%s\" list: %s",
                            name,
                            ib_context_full_get(ctx),
                            ib_status_to_string(rc));
        return rc;
    }
    ib_cfg_log_trace_ex(ib, file, lineno,
                        "Added \"%s\" to context=\"%s\" list",
                        name,
                        ib_context_full_get(ctx));

    return IB_OK;
}

ib_status_t ib_rule_enable_all(const ib_engine_t *ib,
                               ib_context_t *ctx,
                               const char *file,
                               unsigned int lineno)
{
    assert(ib != NULL);
    assert(ctx != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable All",
        rule_enable_all,
        NULL,
        true,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_enable_id(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *id
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable ID",
        rule_enable_by_id,
        (void *)id,
        true,
        true,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_enable_id_prefix(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *id
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable ID Prefix",
        rule_enable_by_id_prefix,
        (void *)id,
        true,
        true,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_disable_id_prefix(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *id
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable ID Prefix",
        rule_enable_by_id_prefix,
        (void *)id,
        false,
        true,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_enable_tag(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *tag
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(tag != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable Tag",
        rule_enable_by_tag,
        (void *)tag,
        true,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_enable_tag_prefix(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *tag
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(tag != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable Tag Prefix",
        rule_enable_by_tag_prefix,
        (void *)tag,
        true,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_disable_tag_prefix(
    const ib_engine_t *ib,
    ib_context_t *ctx,
    const char *file,
    unsigned int lineno,
    const char *tag
)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(tag != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleEnable Tag Prefix",
        rule_enable_by_tag_prefix,
        (void *)tag,
        false,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_disable_all(const ib_engine_t *ib,
                                ib_context_t *ctx,
                                const char *file,
                                unsigned int lineno)
{
    assert(ib != NULL);
    assert(ctx != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleDisable All",
        rule_enable_all,
        NULL,
        false,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_disable_id(const ib_engine_t *ib,
                               ib_context_t *ctx,
                               const char *file,
                               unsigned int lineno,
                               const char *id)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(id != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleDisable ID",
        rule_enable_by_id,
        (void *)id,
        false,
        true,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_disable_tag(const ib_engine_t *ib,
                                ib_context_t *ctx,
                                const char *file,
                                unsigned int lineno,
                                const char *tag)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(tag != NULL);

    ib_status_t rc;

    rc = ib_rule_enable(
        ib,
        ctx,
        "RuleDisable Tag",
        rule_enable_by_tag,
        (void *)tag,
        false,
        false,
        file,
        lineno
    );

    return rc;
}

ib_status_t ib_rule_set_operator(ib_engine_t *ib,
                                 ib_rule_t *rule,
                                 const ib_operator_inst_t *opinst)
{
    assert(ib != NULL);
    assert(rule != NULL);

    if (rule->opinst == NULL) {
        rule->opinst = ib_mm_calloc(
            ib_engine_mm_main_get(ib),
            1, sizeof(*(rule->opinst))
        );
        if (rule->opinst == NULL) {
            return IB_EALLOC;
        }
    }

    rule->opinst->opinst = opinst;

    return IB_OK;
}

ib_status_t ib_rule_set_id(ib_engine_t *ib,
                           ib_rule_t *rule,
                           const char *id)
{
    assert(ib != NULL);
    assert(rule != NULL);

    if ( (rule == NULL) || (id == NULL) ) {
        ib_log_error(ib, "Error setting rule id: Invalid rule or id");
        return IB_EINVAL;
    }

    if (rule->chained_from != NULL) {
        ib_log_error(ib, "Error setting rule id of chained rule");
        return IB_EINVAL;
    }

    if (rule->meta.id != NULL) {
        ib_log_error(ib, "Error setting rule id: Already set to \"%s\"",
                     rule->meta.id);
        return IB_EINVAL;
    }

    rule->meta.id = id;

    return IB_OK;
}

const char *ib_rule_id(const ib_rule_t *rule)
{
    assert(rule != NULL);

    if (rule->meta.full_id != NULL) {
        return rule->meta.full_id;
    }

    if (rule->meta.id != NULL) {
        return rule->meta.id;
    }
    return rule->meta.id;
}

const char *ib_rule_phase_name(ib_rule_phase_num_t phase)
{
    const ib_rule_phase_meta_t *meta;
    ib_status_t rc;

    rc = find_phase_meta(phase, &meta);
    if (rc != IB_OK) {
        return NULL;
    }
    return phase_name(meta);
}

bool ib_rule_id_match(const ib_rule_t *rule, const char *id)
{
    assert(rule != NULL);
    assert(id != NULL);

    /* First match the rule's ID and full ID */
    if ( (strcasecmp(id, rule->meta.id) == 0) ||
         (strcasecmp(id, rule->meta.full_id) == 0) )
    {
        return true;
    }

    /* Check parent rules. */
    for (
        ib_rule_t *parent = rule->chained_from;
        parent != NULL;
        parent = parent->chained_from
    )
    {
        if ( (strcasecmp(id, parent->meta.id) == 0) ||
             (strcasecmp(id, parent->meta.full_id) == 0) )
        {
            return true;
        }
    }

    /* Check child rules. */
    for (
        ib_rule_t *child = rule->chained_rule;
        child != NULL;
        child = child->chained_rule
    )
    {
        if ( (strcasecmp(id, child->meta.id) == 0) ||
             (strcasecmp(id, child->meta.full_id) == 0) )
        {
            return true;
        }
    }

    /* Finally, no match */
    return false;
}

bool ib_rule_tag_match(const ib_rule_t *rule, const char *tag)
{
    const ib_list_node_t *node;

    /* First match the rule's tags */
    IB_LIST_LOOP_CONST(rule->meta.tags, node) {
        const char *ruletag = (const char *)ib_list_node_data_const(node);
        if (strcasecmp(tag, ruletag) == 0) {
            return true;
        }
    }

    /* Check parent rules. */
    for (
        ib_rule_t *parent = rule->chained_from;
        parent != NULL;
        parent = parent->chained_from
    )
    {
        IB_LIST_LOOP_CONST(parent->meta.tags, node) {
            const char *ruletag = (const char *)ib_list_node_data_const(node);
            if (strcasecmp(tag, ruletag) == 0) {
                return true;
            }
        }
    }

    /* Check child rules. */
    for (
        ib_rule_t *child = rule->chained_rule;
        child != NULL;
        child = child->chained_rule
    )
    {
        IB_LIST_LOOP_CONST(child->meta.tags, node) {
            const char *ruletag = (const char *)ib_list_node_data_const(node);
            if (strcasecmp(tag, ruletag) == 0) {
                return true;
            }
        }
    }

    /* Finally, no match */
    return false;
}

ib_status_t ib_rule_create_target(ib_engine_t *ib,
                                  const char *str,
                                  ib_list_t *tfns,
                                  ib_rule_target_t **target)
{
    ib_status_t rc;

    assert(ib != NULL);
    assert(target != NULL);

    /* Allocate a rule field structure */
    *target = (ib_rule_target_t *)
        ib_mm_calloc(ib_rule_mm(ib), sizeof(**target), 1);
    if (*target == NULL) {
        return IB_EALLOC;
    }

    /* Copy the original */
    if (str == NULL) {
        (*target)->target_str = NULL;
        (*target)->target = NULL;
    }
    else {
        /* Acquire target. */
        rc = ib_var_target_acquire_from_string(
            &(*target)->target,
            ib_rule_mm(ib),
            ib_engine_var_config_get_const(ib),
            IB_S2SL(str)
        );
        if (rc != IB_OK) {
            ib_log_error(ib, "Error acquiring target \"%s\": %s",
                         str, ib_status_to_string(rc));
            return IB_EOTHER;
        }

        (*target)->target_str =
            (char *)ib_mm_strdup(ib_rule_mm(ib), str);
        if ((*target)->target_str == NULL) {
            return IB_EALLOC;
        }
    }

    /* Create the field transformation list */
    rc = ib_list_create(&((*target)->tfn_list), ib_rule_mm(ib));
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error creating field operator list for target \"%s\": %s",
                     str, ib_status_to_string(rc));
        return rc;
    }

    /* Add the transformations in the list (if provided) */
    if (tfns != NULL) {
        const ib_list_node_t *node = NULL;

        /* Copy the list elements. */
        IB_LIST_LOOP_CONST(tfns, node) {
            rc = ib_list_push(
                (*target)->tfn_list,
                (void *)ib_list_node_data_const(node));
            if (rc != IB_OK) {
                return rc;
            }
        }
    }

    return IB_OK;
}

/* Add a target to a rule */
ib_status_t ib_rule_add_target(ib_engine_t *ib,
                               ib_rule_t *rule,
                               ib_rule_target_t *target)
{
    ib_status_t rc = IB_OK;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(target != NULL);

    /* Enforce the no target flag */
    if (ib_flags_any(rule->flags, IB_RULE_FLAG_NO_TGT)) {
        ib_log_error(ib,
                     "Error adding target to action rule \"%s\": "
                     "No targets allowed",
                     rule->meta.id);
        return IB_EINVAL;
    }

    /* Push the field */
    rc = ib_list_push(rule->target_fields, (void *)target);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error adding target \"%s\" to rule \"%s\": %s",
                     target->target_str, rule->meta.id,
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/* Add a transformation to a target */
ib_status_t ib_rule_target_add_tfn(ib_engine_t *ib,
                                   ib_rule_target_t *target,
                                   const char *name,
                                   const char *arg)
{

    assert(ib != NULL);
    assert(target != NULL);
    assert(name != NULL);

    ib_status_t          rc;
    const ib_transformation_t *tfn;
    ib_transformation_inst_t *tfn_inst;

    /* Lookup the transformation by name */
    rc = ib_transformation_lookup(ib, IB_S2SL(name), &tfn);
    if (rc == IB_ENOENT) {
        ib_log_error(ib,
                     "Error looking up transformation \"%s\" for target \"%s\": "
                     "Unknown transformation",
                     name, target->target_str);
        return rc;
    }
    else if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error looking up transformation \"%s\" for target \"%s\": %s",
                     name, target->target_str, ib_status_to_string(rc));
        return rc;
    }

    rc = ib_transformation_inst_create(&tfn_inst, ib_rule_mm(ib), tfn, arg);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Failed to create new instance of transformation \"%s\".",
                     name);
        return rc;
    }

    /* Add the transformation to the list */
    rc = ib_list_push(target->tfn_list, (void *)tfn_inst);
    if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error adding transformation \"%s\" to list: %s",
                     name, ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

/* Add a transformation to all targets of a rule */
ib_status_t ib_rule_add_tfn(ib_engine_t *ib,
                            ib_rule_t *rule,
                            const char *name,
                            const char *arg)
{
    ib_status_t rc;
    const ib_transformation_t *tfn;
    ib_list_node_t *node = NULL;

    assert(ib != NULL);
    assert(rule != NULL);
    assert(name != NULL);

    /* Lookup the transformation by name */
    rc = ib_transformation_lookup(ib, IB_S2SL(name), &tfn);
    if (rc == IB_ENOENT) {
        ib_log_error(ib,
                     "Error looking up transformation \"%s\" for rule \"%s\": "
                     "Unknown transformation",
                     name, rule->meta.id);
        return rc;
    }
    else if (rc != IB_OK) {
        ib_log_error(ib,
                     "Error looking up transformation \"%s\" for rule \"%s\": %s",
                     name, rule->meta.id, ib_status_to_string(rc));
        return rc;
    }

    /* Walk through the list of targets, add the transformation to it */
    IB_LIST_LOOP(rule->target_fields, node) {
        ib_rule_target_t *target = (ib_rule_target_t *)ib_list_node_data(node);
        rc = ib_rule_target_add_tfn(ib, target, name, arg);
        if (rc != IB_OK) {
            ib_log_notice(ib,
                          "Error adding transformation \"%s\" to target \"%s\" "
                          "rule \"%s\"",
                          name, target->target_str, rule->meta.id);
        }
    }

    return IB_OK;
}

/* Check a rule (action) parameters */
ib_status_t ib_rule_check_params(ib_engine_t *ib,
                                 ib_rule_t *rule,
                                 const char *params)
{
    assert(ib != NULL);
    assert(rule != NULL);

    /* No parameters -> Nothing to do */
    if (params == NULL) {
        return IB_OK;
    }

    /* Look for a FIELD string in the action's parameters string */
    if ( (strcasestr(params, "%{FIELD") != NULL) ||
         (strcasecmp(params, "FIELD") == 0) ||
         (strcasecmp(params, "FIELD_TARGET") == 0) ||
         (strcasecmp(params, "FIELD_TFN") == 0) ||
         (strcasecmp(params, "FIELD_NAME") == 0) ||
         (strcasecmp(params, "FIELD_NAME_FULL") == 0) )
    {
        ib_flags_set(rule->flags, IB_RULE_FLAG_FIELDS);
    }
    return IB_OK;
}

/* Add an action to a rule */
ib_status_t ib_rule_add_action(ib_engine_t *ib,
                               ib_rule_t *rule,
                               ib_action_inst_t *action,
                               ib_rule_action_t which)
{
    assert(ib != NULL);

    ib_status_t rc;
    ib_list_t  *actions;

    if ( (rule == NULL) || (action == NULL) ) {
        ib_log_error(ib,
                     "Error adding rule action: Invalid rule or action");
        return IB_EINVAL;
    }

    /* Selection the appropriate action list */
    switch (which) {
    case IB_RULE_ACTION_TRUE :
        actions = rule->true_actions;
        break;
    case IB_RULE_ACTION_FALSE :
        actions = rule->false_actions;
        break;
    case IB_RULE_ACTION_AUX :
        actions = rule->aux_actions;
        break;
    default:
        return IB_EINVAL;
    }

    /* Some actions require IB_RULE_FLAG_FIELDS to be set.
     * FIXME: This is fragile code. Event should be able to construct
     *        the current field name from the provided rule_exec. */
    if (strcasestr(ib_action_name(ib_action_inst_action(action)), "event") != NULL) {
        ib_flags_set(rule->flags, IB_RULE_FLAG_FIELDS);
    }

    /* Add the action to the list */
    rc = ib_list_push(actions, (void *)action);
    if (rc != IB_OK) {
        ib_log_error(ib, "Error adding rule action \"%s\": %s",
                     ib_action_name(ib_action_inst_action(action)),
                     ib_status_to_string(rc));
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_rule_search_action(const ib_engine_t *ib,
                                  const ib_rule_t *rule,
                                  ib_rule_action_t which,
                                  const char *name,
                                  ib_list_t *matches,
                                  size_t *pcount)
{
    if ((ib == NULL) || (rule == NULL) || (name == NULL) ) {
        return IB_EINVAL;
    }
    if ( (matches == NULL) && (pcount == NULL) ) {
        return IB_EINVAL;
    }
    const ib_list_node_t *node;
    const ib_list_t *list;
    size_t count = 0;

    list = ( (which == IB_RULE_ACTION_TRUE) ?
             rule->true_actions : rule->false_actions);

    IB_LIST_LOOP_CONST(list, node) {
        ib_action_inst_t *inst =
            (ib_action_inst_t *)ib_list_node_data_const(node);
        assert(inst != NULL);
        const ib_action_t *action = ib_action_inst_action(inst);
        assert(action != NULL);
        const char *action_name = ib_action_name(action);
        assert(action_name != NULL);
        if (strcmp(action_name, name) == 0) {
            ++count;
            if (matches != NULL) {
                ib_list_push(matches, inst);
            }
        }
    }

    if (pcount != NULL) {
        *pcount = count;
    }

    return IB_OK;
}

ib_status_t ib_rule_set_capture(
    ib_engine_t *ib,
    ib_rule_t   *rule,
    const char  *capture_collection)
{
    if ( (ib == NULL) || (rule == NULL) ) {
        return IB_EINVAL;
    }

    assert(rule->opinst != NULL);

    /* If the operator doesn't support capture, return an error */
    if (
        ! ib_flags_any(
            ib_operator_capabilities(ib_operator_inst_operator(rule->opinst->opinst)),
            IB_OP_CAPABILITY_CAPTURE
        )
    ) {
        return IB_ENOTIMPL;
    }

    /* Turn on the capture flag */
    rule->flags |= IB_RULE_FLAG_CAPTURE;

    /* Copy the collection name */
    if ( (capture_collection != NULL) && (*capture_collection != '\0') ) {
        rule->capture_collection =
            ib_mm_strdup(ib_rule_mm(ib), capture_collection);
        if (rule->capture_collection == NULL) {
            return IB_EALLOC;
        }
    }

    return IB_OK;
}

ib_status_t ib_rule_chain_invalidate(ib_engine_t *ib,
                                     ib_context_t *ctx,
                                     ib_rule_t *rule)
{
    ib_status_t rc = IB_OK;
    ib_status_t tmp_rc;
    ib_flags_t orig;

    if (rule == NULL) {
        return IB_EINVAL;
    }
    orig = rule->flags;
    rule->flags &= ~IB_RULE_FLAG_VALID;

    /* Invalidate the entire chain upwards */
    if (rule->chained_from != NULL) {
        tmp_rc = ib_rule_chain_invalidate(ib, NULL, rule->chained_from);
        if (tmp_rc != IB_OK) {
            rc = tmp_rc;
        }
    }

    /* If this rule was previously valid, walk down the chain, too */
    if ( (orig & IB_RULE_FLAG_VALID) && (rule->chained_rule != NULL) ) {
        tmp_rc = ib_rule_chain_invalidate(ib, NULL, rule->chained_rule);
        if (tmp_rc != IB_OK) {
            rc = tmp_rc;
        }
    }

    /* Clear the previous rule pointer */
    if (ctx != NULL) {
        ctx->rules->parser_data.previous = NULL;
    }

    return rc;
}

static IB_STRVAL_MAP(debug_levels_map) = {
    IB_STRVAL_PAIR("error", IB_RULE_DLOG_ERROR),
    IB_STRVAL_PAIR("warning", IB_RULE_DLOG_WARNING),
    IB_STRVAL_PAIR("notice", IB_RULE_DLOG_NOTICE),
    IB_STRVAL_PAIR("info", IB_RULE_DLOG_INFO),
    IB_STRVAL_PAIR("debug", IB_RULE_DLOG_DEBUG),
    IB_STRVAL_PAIR("trace", IB_RULE_DLOG_TRACE),
    IB_STRVAL_PAIR_LAST
};

ib_status_t ib_rule_engine_set(ib_cfgparser_t *cp,
                               const char *name,
                               const char *value)
{
    assert(cp != NULL);
    assert(name != NULL);
    assert(value != NULL);
    ib_status_t rc;

    if (strcasecmp(name, "RuleEngineDebugLogLevel") == 0) {
        ib_num_t tmp;
        ib_num_t level;

        if (sscanf(value, "%" SCNd64, &tmp) != 0) {
            level = tmp;
        }
        else {
            rc = ib_config_strval_pair_lookup(value, debug_levels_map, &level);
            if (rc != IB_OK) {
                return rc;
            }
        }
        rc = ib_context_set_num(cp->cur_ctx, "_RuleEngineDebugLevel", level);
        return rc;
    }

    return IB_EINVAL;
}

ib_status_t ib_rule_register_external_driver(
    ib_engine_t               *ib,
    const char                *tag,
    ib_rule_driver_fn_t        function,
    void                      *cbdata)
{
    /* Check for invalid parameters */
    if ( (ib == NULL) || (tag == NULL) || (function == NULL) ) {
        return IB_EINVAL;
    }

    /* Verify that the rule engine is valid */
    assert(ib->rule_engine != NULL);
    assert(ib->rule_engine->external_drivers != NULL);

    ib_status_t rc;
    ib_rule_driver_t *driver;

    rc = ib_hash_get(ib->rule_engine->external_drivers, &driver, tag);
    if (rc != IB_ENOENT) {
        return IB_EINVAL;
    }

    driver = ib_mm_calloc(ib_engine_mm_main_get(ib), 1, sizeof(*driver));
    if (driver == NULL) {
        return IB_EALLOC;
    }
    driver->function = function;
    driver->cbdata   = cbdata;

    rc = ib_hash_set(ib->rule_engine->external_drivers, tag, driver);
    if (rc == IB_EALLOC) {
        return IB_EALLOC;
    }
    else if (rc != IB_OK) {
        return IB_EOTHER;
    }

    return IB_OK;
}

ib_status_t ib_rule_lookup_external_driver(
    const ib_engine_t         *ib,
    const char                *tag,
    ib_rule_driver_t         **driver)
{
    if ( (ib == NULL) || (tag == NULL) || (driver == NULL) ) {
        return IB_EINVAL;
    }
    assert(ib->rule_engine != NULL);
    assert(ib->rule_engine->external_drivers != NULL);

    ib_status_t rc;

    rc = ib_hash_get(ib->rule_engine->external_drivers, driver, tag);
    return rc;
}

ib_status_t ib_rule_register_ownership_fn(
    ib_engine_t            *ib,
    const char             *name,
    ib_rule_ownership_fn_t  ownership_fn,
    void                   *cbdata)
{
    if ( (ib == NULL) || (name == NULL) || (ownership_fn == NULL) ) {
        return IB_EINVAL;
    }
    assert(ib->rule_engine != NULL);
    assert(ib->rule_engine->ownership_cbs != NULL);

    ib_status_t rc;
    ib_rule_ownership_cb_t *cb;
    ib_mm_t mm = ib->rule_engine->ownership_cbs->mm;

    cb = ib_mm_alloc(mm, sizeof(*cb));
    if (cb == NULL) {
        return IB_EALLOC;
    }
    cb->name = ib_mm_strdup(mm, name);
    if (cb->name == NULL) {
        return IB_EALLOC;
    }
    cb->fn = ownership_fn;
    cb->data = cbdata;

    rc = ib_list_push(ib->rule_engine->ownership_cbs, cb);

    return rc;
}

ib_status_t ib_rule_register_injection_fn(
    ib_engine_t            *ib,
    const char             *name,
    ib_rule_phase_num_t     phase,
    ib_rule_injection_fn_t  injection_fn,
    void                   *cbdata)
{
    if ( (ib == NULL) || (name == NULL) || (injection_fn == NULL) ) {
        return IB_EINVAL;
    }
    assert(ib->rule_engine != NULL);
    assert(ib->rule_engine->injection_cbs != NULL);

    ib_status_t rc;
    ib_rule_injection_cb_t *cb;
    ib_list_t *cb_list;
    ib_mm_t mm;

    if (! is_phase_num_valid(phase)) {
        return IB_EINVAL;
    }
    cb_list = ib->rule_engine->injection_cbs[phase];
    assert(cb_list != NULL);
    mm = cb_list->mm;

    cb = ib_mm_alloc(mm, sizeof(*cb));
    if (cb == NULL) {
        return IB_EALLOC;
    }
    cb->name = ib_mm_strdup(mm, name);
    if (cb->name == NULL) {
        return IB_EALLOC;
    }
    cb->fn = injection_fn;
    cb->data = cbdata;

    rc = ib_list_push(ib->rule_engine->injection_cbs[phase], cb);

    return rc;
}

ib_status_t ib_rule_register_pre_rule_fn(
    ib_engine_t *ib,
    ib_rule_pre_rule_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_pre_rule_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.pre_rule;

    hook = (ib_rule_pre_rule_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_register_post_rule_fn(
    ib_engine_t *ib,
    ib_rule_post_rule_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_post_rule_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.post_rule;

    hook = (ib_rule_post_rule_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_register_pre_operator_fn(
    ib_engine_t *ib,
    ib_rule_pre_operator_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_pre_operator_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.pre_operator;

    hook = (ib_rule_pre_operator_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_register_post_operator_fn(
    ib_engine_t *ib,
    ib_rule_post_operator_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_post_operator_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.post_operator;

    hook = (ib_rule_post_operator_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_register_pre_action_fn(
    ib_engine_t *ib,
    ib_rule_pre_action_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_pre_action_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.pre_action;

    hook = (ib_rule_pre_action_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_register_post_action_fn(
    ib_engine_t *ib,
    ib_rule_post_action_fn_t fn,
    void *cbdata
)
{
    assert(ib != NULL);
    assert(fn != NULL);

    ib_rule_post_action_hook_t *hook;
    ib_list_t *hook_list = ib->rule_engine->hooks.post_action;

    hook = (ib_rule_post_action_hook_t *)ib_mm_alloc(hook_list->mm, sizeof(*hook));
    if (hook == NULL) {
        return IB_EALLOC;
    }
    hook->fn = fn;
    hook->data = cbdata;

    return ib_list_push(hook_list, hook);
}

ib_status_t ib_rule_tfn_fields_to_inst(
    ib_engine_t *ib,
    ib_mm_t      mm,
    ib_list_t   *tfn_fields,
    ib_list_t   *tfn_insts
)
{
    assert(ib != NULL);
    assert(tfn_fields != NULL);
    assert(tfn_insts != NULL);

    const ib_list_node_t *node;
    bool  transformations_missing = false;

    IB_LIST_LOOP_CONST(tfn_fields, node) {
        ib_status_t          rc;
        const ib_transformation_t *tfn;
        ib_transformation_inst_t *tfn_inst;
        const char          *tfn_arg;
        const ib_field_t    *tfn_field =
            (const ib_field_t *)ib_list_node_data_const(node);

        assert(tfn_field != NULL);

        rc = ib_transformation_lookup(
            ib,
            tfn_field->name, tfn_field->nlen,
            &tfn
        );
        if (rc == IB_ENOENT) {
            /* report error*/
            ib_log_error(
                ib,
                "Cannot find transformation \"%.*s\".",
                (int)tfn_field->nlen,
                tfn_field->name);
            transformations_missing = true;
            continue;
        }
        else if (rc != IB_OK) {
            return rc;
        }

        rc = ib_field_value_type(
            tfn_field,
            ib_ftype_nulstr_out(&tfn_arg),
            IB_FTYPE_NULSTR);
        if (rc != IB_OK) {
            ib_log_error(
                ib,
                "Invalid field type. Cannot extract transformation argument.");
            return rc;
        }

        rc = ib_transformation_inst_create(&tfn_inst, mm, tfn, tfn_arg);
        if (rc !=  IB_OK) {
            return rc;
        }

        rc = ib_list_push(tfn_insts, (void *)tfn_inst);
        if (rc != IB_OK) {
            return rc;
        }
    }

    if (transformations_missing) {
        return IB_ENOENT;
    }

    return IB_OK;
}
