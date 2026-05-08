#include "argon2_kdf.h"

#include "argon2.h"
#include "core.h"

static int qos_argon2_ctx(argon2_context* context, argon2_type type){
    int result;
    unsigned int memory_blocks;
    unsigned int segment_length;
    argon2_instance_t instance;

    if (!context){
        return ARGON2_INCORRECT_PARAMETER;
    }

    result = validate_inputs(context);
    if (result != ARGON2_OK){
        return result;
    }

    if (type != Argon2_d && type != Argon2_i && type != Argon2_id){
        return ARGON2_INCORRECT_TYPE;
    }

    memory_blocks = context->m_cost;
    if (memory_blocks < 2u * ARGON2_SYNC_POINTS * context->lanes){
        memory_blocks = 2u * ARGON2_SYNC_POINTS * context->lanes;
    }

    segment_length = memory_blocks / (context->lanes * ARGON2_SYNC_POINTS);
    memory_blocks = segment_length * (context->lanes * ARGON2_SYNC_POINTS);

    instance.version = context->version;
    instance.memory = 0;
    instance.passes = context->t_cost;
    instance.memory_blocks = memory_blocks;
    instance.segment_length = segment_length;
    instance.lane_length = segment_length * ARGON2_SYNC_POINTS;
    instance.lanes = context->lanes;
    instance.threads = context->threads;
    instance.type = type;
    instance.context_ptr = context;
    instance.print_internals = 0;

    if (instance.threads > instance.lanes){
        instance.threads = instance.lanes;
    }

    result = initialize(&instance, context);
    if (result != ARGON2_OK){
        if (instance.memory){
            free_memory(context, (unsigned char*)instance.memory, instance.memory_blocks, sizeof(block));
        }
        return result;
    }

    result = fill_memory_blocks(&instance);
    if (result != ARGON2_OK){
        if (instance.memory){
            free_memory(context, (unsigned char*)instance.memory, instance.memory_blocks, sizeof(block));
        }
        return result;
    }

    finalize(context, &instance);
    return ARGON2_OK;
}

int argon2id_hash_raw_qos(unsigned int t_cost,
                          unsigned int m_cost_kib,
                          unsigned int parallelism,
                          const void* password,
                          unsigned int password_len,
                          const void* salt,
                          unsigned int salt_len,
                          void* out_hash,
                          unsigned int out_hash_len,
                          unsigned int version){
    argon2_context context;

    if (!out_hash){
        return ARGON2_OUTPUT_PTR_NULL;
    }
    if (out_hash_len < ARGON2_MIN_OUTLEN){
        return ARGON2_OUTPUT_TOO_SHORT;
    }
    if (out_hash_len > ARGON2_MAX_OUTLEN){
        return ARGON2_OUTPUT_TOO_LONG;
    }
    if (password_len > ARGON2_MAX_PWD_LENGTH){
        return ARGON2_PWD_TOO_LONG;
    }
    if (salt_len < ARGON2_MIN_SALT_LENGTH){
        return ARGON2_SALT_TOO_SHORT;
    }
    if (salt_len > ARGON2_MAX_SALT_LENGTH){
        return ARGON2_SALT_TOO_LONG;
    }

    context.out = (unsigned char*)out_hash;
    context.outlen = out_hash_len;
    context.pwd = (unsigned char*)password;
    context.pwdlen = password_len;
    context.salt = (unsigned char*)salt;
    context.saltlen = salt_len;
    context.secret = 0;
    context.secretlen = 0;
    context.ad = 0;
    context.adlen = 0;
    context.t_cost = t_cost;
    context.m_cost = m_cost_kib;
    context.lanes = parallelism;
    context.threads = parallelism;
    context.version = version;
    context.allocate_cbk = 0;
    context.free_cbk = 0;
    context.flags = ARGON2_DEFAULT_FLAGS;

    return qos_argon2_ctx(&context, Argon2_id);
}
