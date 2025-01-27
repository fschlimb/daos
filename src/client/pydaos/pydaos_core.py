# (C) Copyright  2019 Intel Corporation.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
# The Government's rights to use, modify, reproduce, release, perform, display,
# or disclose this software are subject to the terms of the Apache License as
# provided in Contract No. B609815.
# Any reproduction of computer software, computer software documentation, or
# portions thereof marked with this legend must also reproduce the markings.
"""
PyDAOS Module allowing global access to the DAOS containers and objects.
"""

import enum
import uuid
import sys

# pylint: disable=import-error
if sys.version_info < (3, 0):
    import pydaos_shim_27 as pydaos_shim
else:
    import pydaos_shim_3 as pydaos_shim
# pylint: enable=import-error

# Import Object class as an enumeration
ObjClassID = enum.Enum(
    "Enumeration of the DAOS object classes (OC).",
    {key: value for key, value in pydaos_shim.__dict__.items()
    if key.startswith("OC_")})

class ObjID(object):
    """
    Class representing of DAOS 128-bit object identifier

    Attributes
    ----------
    hi : int
        high 64 bits of the object identifier
    lo : int
        low 64 bits of the object identifier

    Methods
    -------
    __str__
        print object ID in hexa [hi:lo]
    """
    def __init__(self, hi, lo):
        self.hi = hi
        self.lo = lo

    def __str__(self):
        return "[" + hex(self.hi) + ":" + hex(self.lo) + "]"

class Cont(object):
    """
    Class representing of DAOS Container
    Can be identified via a path or a combination of pool UUID and container
    UUID. DAOS pool and container are opened during the __init__ phase and
    closed on __del__.

    Attributes
    ----------
    path : string
        Path for container representation in unified namespace
    puuid : uuid
        Pool uuid, parsed via uuid.UUID(puuid)
    cuuid : uuid
        Container uuid, parsed via uuid.UUID(cuuid)

    Methods
    -------
    genoid(cid)
        Generate a new object ID globally unique for this container.
        cid must be on type ObjClassID and identify the object class to use for
        the new object. Upon success, a python object of type ObjID is returned.
    rootkv(cid = ObjClassID.OC_SX)
        Open the container root key-value store.
        Default object class can be modified but should remain always the same.
        Upon success, a python object of type KVObj is returned.
    newkv(cid = ObjClassID.OC_SX)
        Allocate a new key-value store of class cid.
        Upon success, a python object of type KVObj is returned.
    kv(oid)
        Open an already-allocated object identified by oid of type ObjID.
        Upon success, a python object of type KVObj is returned.
    __str__
        print pool and container UUIDs
    """
    def __init__(self, puuid = None, cuuid = None, path = None):
        if path == None and (puuid == None or cuuid == None):
            raise PyDError("invalid pool or container UUID",
                           -pydaos_shim.DER_INVAL)
        if path != None:
            (ret, poh, coh) = pydaos_shim.cont_open_by_path(DAOS_MAGIC, path),
        else:
            self.puuid = uuid.UUID(puuid)
            self.cuuid = uuid.UUID(cuuid)
            (ret, poh, coh) = pydaos_shim.cont_open(DAOS_MAGIC, str(puuid),
                                                    str(cuuid), 0)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to access container", ret)
        self.poh = poh
        self.coh = coh

    def __del__(self):
        ret = pydaos_shim.cont_close(DAOS_MAGIC, self.poh, self.coh)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to close container", ret)

    def genoid(self, cid):
        (ret, hi, lo) = pydaos_shim.obj_idgen(DAOS_MAGIC, self.coh, cid.value)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to generate object identifier", ret)
        return ObjID(hi, lo)

    def newkv(self, cid = ObjClassID.OC_SX):
        oid = self.genoid(cid)
        return KVObj(self.coh, oid, self)

    def kv(self, oid):
        return KVObj(self.coh, oid, self)

    def rootkv(self, cid = ObjClassID.OC_SX):
        (ret, hi, lo) = pydaos_shim.obj_idroot(DAOS_MAGIC, cid.value)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to generate root object identifier", ret)
        oid = ObjID(hi, lo)
        return KVObj(self.coh, oid, self)

    def __str__(self):
        return str(self.cuuid) + "@" + str(self.puuid)

class _Obj(object):
    oh = None

    def __init__(self, coh, oid, cont):
        self.oid = oid
        # Open to the object
        (ret, oh) = pydaos_shim.obj_open(DAOS_MAGIC, coh, self.oid.hi,
                                         self.oid.lo, 0)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to open object", ret)
        self.oh = oh
        # keep container around until all objects are gone
        self.cont = cont

    def __del__(self):
        if self.oh is None:
            return
        ret = pydaos_shim.obj_close(DAOS_MAGIC, self.oh)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to close object", ret)

    def getoid(self):
        return self.oid

    def __str__(self):
        return str(self.oid)

class KVObj(_Obj):
    """
    Class representing of DAOS key-value (KV) store object
    As all DAOS objects, a KV object is identified by a unique 128-bit
    identifier represented by the class ObjID.
    Only strings are supported for both the key and value for now.
    Key-value pair can be inserted/looked up once at a time (see put/get) or
    in bulk (see bput/bget) taking a python dict as an input. The bulk
    operations are issued in parallel (up to 16 operations in flight) to
    maximize the operation rate.
    Key-value pair are deleted via the put/bput operations by setting the value
    to either None or the empty string. Once deleted, the key won't be reported
    during iteration.
    The DAOS KV objects behave like a python dictionary and supports:
    - 'dkv[key]' which invokes 'dkv.get(key)'
    - 'dkv[key] = val' which invokes 'dkv.put(key, val)'
    - 'for key in dkv:' allows to walk through the key space via the support of
      python iterators
    - 'if key is in dkv:' allows to test whether a give key is present in the
      DAOS KV store.
    - 'len(dkv)' returns the number of key-value pairs
    - 'bool(dkv)' reports 'False' if there is no key-value pairs in the DAOS KV
      and 'True' otherwise.

    Python iterators are supported, which means that "for key in kvobj:" will
    allow you to walk through the key space.
    For each method, a PyDError exception is raised with proper DAOS error code
    (in string format) if the operation cannot be completed.

    Methods
    -------
    get(key)
        Retrieve value associated with the key.
        If found, the string value is returned, None is returned otherwise.
    put(key, val)
        Update/insert key-value pair. Both parameters should be strings.
    bget(ddict)
        Bulk get value for all the keys of the input python dictionary.
        Get operations are issued in parallel over the network.
        The existing value in ddict is overwritten with the value retrieved from
        DAOS. If the key isn't found, the value is set to None.
    bput(ddict)
        Bulk put all the key-value pairs of the input python dictionary.
        Put operations are issued in parallel over the network.
        If the value is set to None or an empty string, the key is deleted from
        the DAOS KV store.
    dump()
        Fetch all the key-value pairs and return them in a python dictionary.
    """
    def get(self, key):
        d = { key : None }
        self.bget(d)
        return d[key]

    def __getitem__(self, key):
        return self.get(key)

    def put(self, key, val):
        d = { key : val }
        self.bput(d)

    def __setitem__(self, key, val):
        self.put(key, val)

    def bget(self, ddict):
        ret = pydaos_shim.kv_get(DAOS_MAGIC, self.oh, ddict)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to retrieve KV value", ret)

    def bput(self, ddict):
        ret = pydaos_shim.kv_put(DAOS_MAGIC, self.oh, ddict)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to store KV value", ret)

    def dump(self):
        # leverage python iterator, see __iter__/__next__ below
        # could be optimized over the C library in the future
        d = {}
        for key in self:
            d[key] = None
        self.bget(d)
        return d

    def __len__(self):
        # Not efficient for now. Fetch all keys and count them.
        i = 0
        for key in self:
	    i += 1
	return i

    def __bool__(self):
        for key in self:
            return True
	return False

    def __contains__(self, key):
        if self.get(key) == None:
	    return False
	return True

    def __iter__(self):
        self.entries = []
        self.nr = 256
        self.size = 4096 # optimized for 16-char strings
        self.anchor = None
        self.done = False
        return self

    def next(self):
        # for python 2 compatibility
        return self.__next__()

    def __next__(self):
        if len(self.entries) != 0:
            return self.entries.pop()
        if self.done:
            raise StopIteration()

        # read more entries
        (ret, nr, sz, anchor) = pydaos_shim.kv_iter(DAOS_MAGIC, self.oh,
                                                    self.entries, self.nr,
                                                    self.size, self.anchor)
        if ret != pydaos_shim.DER_SUCCESS:
            raise PyDError("failed to enumerate KV pair", ret)

        # save param for next iterations, those have been adjusted already by
        # the shim layer
        self.anchor = anchor
        self.nr = nr
        self.size = sz
        if self.anchor == None:
            # no more entries to consume
            self.done = True

        if len(self.entries) != 0:
            return self.entries.pop()
        else:
            raise StopIteration()
