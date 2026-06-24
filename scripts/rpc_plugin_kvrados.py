#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM, Inc.
#
# rpc.py plugin exposing the OUT-OF-TREE bdev_kvrados RPCs (Slice I).
#
# The KV bdev forwarder (target/bdev_kvrados.c) is loaded by the custom nkv_tgt
# app and self-registers `bdev_kvrados_create` / `bdev_kvrados_delete` over
# JSON-RPC. The UNMODIFIED base SPDK's scripts/rpc.py has no built-in subcommand
# for these (they used to be generated from an in-tree schema.json), so this
# plugin adds them. Field names mirror target/bdev_kvrados_rpc.c's decoders.
#
#   rpc.py --plugin rpc_plugin_kvrados bdev_kvrados_create -b KvRados0 \
#          --executor-endpoint ofi+tcp://127.0.0.1:1234
#   rpc.py --plugin rpc_plugin_kvrados bdev_kvrados_delete KvRados0
#
# Make the module importable by pointing PYTHONPATH at this directory (the
# bring-up script and entrypoint do that).

from spdk.rpc.cmd_parser import print_json


def _bdev_kvrados_create(args):
    params = {'name': args.name}
    if args.uuid is not None:
        params['uuid'] = args.uuid
    if args.max_key_size is not None:
        params['max_key_size'] = args.max_key_size
    if args.max_value_size is not None:
        params['max_value_size'] = args.max_value_size
    if args.optimal_value_granularity is not None:
        params['optimal_value_granularity'] = args.optimal_value_granularity
    if args.num_keys is not None:
        params['num_keys'] = args.num_keys
    if args.executor_endpoint is not None:
        params['executor_endpoint'] = args.executor_endpoint
    if args.read_only:
        params['read_only'] = True
    if args.numa_id is not None:
        params['numa_id'] = args.numa_id
    return args.client.call('bdev_kvrados_create', params)


def bdev_kvrados_create(args):
    print_json(_bdev_kvrados_create(args))


def bdev_kvrados_delete(args):
    params = {'name': args.name}
    return args.client.call('bdev_kvrados_delete', params)


def spdk_rpc_plugin_initialize(subparsers):
    p = subparsers.add_parser('bdev_kvrados_create',
                              help='Create a kvrados KV forwarder bdev')
    p.add_argument('-b', '--name', help='bdev name', required=True)
    p.add_argument('-u', '--uuid', help='UUID for the bdev', default=None)
    p.add_argument('--max-key-size', type=int, default=None,
                   help='max key size advertised in KV IDENTIFY')
    p.add_argument('--max-value-size', type=int, default=None,
                   help='max value size advertised in KV IDENTIFY')
    p.add_argument('--optimal-value-granularity', type=int, default=None,
                   help='optimal value granularity advertised in KV IDENTIFY')
    p.add_argument('--num-keys', type=int, default=None,
                   help='number of keys advertised in KV IDENTIFY')
    p.add_argument('--executor-endpoint', default=None,
                   help='rados-nkvx executor NA address (Mercury); '
                        'KV verbs are forwarded here')
    p.add_argument('--read-only', action='store_true',
                   help='loader-style namespace (Retrieve/Exist only)')
    p.add_argument('--numa-id', type=int, default=None, help='NUMA node id')
    p.set_defaults(func=bdev_kvrados_create)

    p = subparsers.add_parser('bdev_kvrados_delete',
                              help='Delete a kvrados forwarder bdev')
    p.add_argument('name', help='kvrados bdev name')
    p.set_defaults(func=bdev_kvrados_delete)
