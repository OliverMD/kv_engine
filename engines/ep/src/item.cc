/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include "ep_time.h"
#include "item.h"
#include "objectregistry.h"

#include <cJSON.h>
#include <platform/compress.h>

#include  <iomanip>

std::atomic<uint64_t> Item::casCounter(1);
const uint32_t Item::metaDataSize(2*sizeof(uint32_t) + 2*sizeof(uint64_t) + 2);

Item::Item(const DocKey& k,
           const uint32_t fl,
           const time_t exp,
           const value_t& val,
           uint64_t theCas,
           int64_t i,
           uint16_t vbid,
           uint64_t sno,
           uint8_t nru_value)
    : metaData(theCas, sno, fl, exp),
      value(val),
      key(k),
      bySeqno(i),
      queuedTime(ep_current_time()),
      vbucketId(vbid),
      op(k.getDocNamespace() == DocNamespace::System ? queue_op::system_event
                                                     : queue_op::set),
      nru(nru_value) {
    if (bySeqno == 0) {
        throw std::invalid_argument("Item(): bySeqno must be non-zero");
    }
    // Update the cached version of the datatype
    if (haveExtMetaData()) {
        datatype = value->getDataType();
    }

    ObjectRegistry::onCreateItem(this);
}

Item::Item(const DocKey& k,
           const uint32_t fl,
           const time_t exp,
           const void* dta,
           const size_t nb,
           uint8_t* ext_meta,
           uint8_t ext_len,
           uint64_t theCas,
           int64_t i,
           uint16_t vbid,
           uint64_t sno,
           uint8_t nru_value)
    : metaData(theCas, sno, fl, exp),
      key(k),
      bySeqno(i),
      queuedTime(ep_current_time()),
      vbucketId(vbid),
      op(k.getDocNamespace() == DocNamespace::System ? queue_op::system_event
                                                     : queue_op::set),
      nru(nru_value) {
    if (bySeqno == 0) {
        throw std::invalid_argument("Item(): bySeqno must be non-zero");
    }
    setData(static_cast<const char*>(dta), nb, ext_meta, ext_len);

    ObjectRegistry::onCreateItem(this);
}

Item::Item(const DocKey& k,
           const uint16_t vb,
           queue_op o,
           const uint64_t revSeq,
           const int64_t bySeq,
           uint8_t nru_value)
    : metaData(),
      key(k),
      bySeqno(bySeq),
      queuedTime(ep_current_time()),
      vbucketId(vb),
      op(o),
      nru(nru_value) {
    if (bySeqno < 0) {
        throw std::invalid_argument("Item(): bySeqno must be non-negative");
    }
    metaData.revSeqno = revSeq;
    ObjectRegistry::onCreateItem(this);
}

Item::Item(const Item& other, bool copyKeyOnly)
    : metaData(other.metaData),
      key(other.key),
      bySeqno(other.bySeqno.load()),
      queuedTime(other.queuedTime),
      vbucketId(other.vbucketId),
      op(other.op),
      nru(other.nru) {
    if (copyKeyOnly) {
        setData(nullptr, 0, nullptr, 0);
    } else {
        value = other.value;
        datatype = other.datatype;
    }
    ObjectRegistry::onCreateItem(this);
}

Item::~Item() {
    ObjectRegistry::onDeleteItem(this);
}

std::string to_string(queue_op op) {
    switch(op) {
        case queue_op::set: return "set";
        case queue_op::del: return "del";
        case queue_op::flush: return "flush";
        case queue_op::empty: return "empty";
        case queue_op::checkpoint_start: return "checkpoint_start";
        case queue_op::checkpoint_end: return "checkpoint_end";
        case queue_op::set_vbucket_state: return "set_vbucket_state";
        case queue_op::system_event: return "system_event";
    }
    return "<" +
            std::to_string(static_cast<std::underlying_type<queue_op>::type>(op)) +
            ">";

}

bool operator==(const Item& lhs, const Item& rhs) {
    return (lhs.metaData == rhs.metaData) &&
           (*lhs.value == *rhs.value) &&
           (lhs.key == rhs.key) &&
           (lhs.bySeqno == rhs.bySeqno) &&
           // Note: queuedTime is *not* compared. The rationale is it is
           // simply used for stats (measureing queue duration) and hence can
           // be ignored from an "equivilence" pov.
           // (lhs.queuedTime == rhs.queuedTime) &&
           (lhs.vbucketId == rhs.vbucketId) &&
           (lhs.op == rhs.op) &&
           (lhs.nru == rhs.nru);
}

std::ostream& operator<<(std::ostream& os, const Item& i) {
    os << "Item[" << &i << "] with"
       << " key:" << i.key << "\n"
       << "\tvalue:" << *i.value << "\n"
       << "\tmetadata:" << i.metaData << "\n"
       << "\tbySeqno:" << i.bySeqno
       << " queuedTime:" << i.queuedTime
       << " vbucketId:" << i.vbucketId
       << " op:" << to_string(i.op)
       << " nru:" << int(i.nru);
    return os;
}

bool operator==(const ItemMetaData& lhs, const ItemMetaData& rhs) {
    return (lhs.cas == rhs.cas) &&
           (lhs.revSeqno == rhs.revSeqno) &&
           (lhs.flags == rhs.flags) &&
           (lhs.exptime == rhs.exptime);
}

std::ostream& operator<<(std::ostream& os, const ItemMetaData& md) {
    os << "ItemMetaData[" << &md << "] with"
       << " cas:" << md.cas
       << " revSeqno:" << md.revSeqno
       << " flags:" << md.flags
       << " exptime:" << md.exptime;
    return os;
}

bool operator==(const Blob& lhs, const Blob& rhs) {
    return (lhs.size == rhs.size) &&
           (lhs.extMetaLen == rhs.extMetaLen) &&
           (lhs.age == rhs.age) &&
           (memcmp(lhs.data, rhs.data, lhs.size) == 0);
}

std::ostream& operator<<(std::ostream& os, const Blob& b) {
    os << "Blob[" << &b << "] with"
       << " size:" << b.size
       << " extMetaLen:" << int(b.extMetaLen)
       << " age:" << int(b.age)
       << " data: <" << std::hex;
    // Print at most 40 bytes of the body.
    auto bytes_to_print = std::min(uint32_t(40), b.size);
    for (size_t ii = 0; ii < bytes_to_print; ii++) {
        if (ii != 0) {
            os << ' ';
        }
        if (isprint(b.data[ii])) {
            os << b.data[ii];
        } else {
            os << std::setfill('0') << std::setw(2) << int(uint8_t(b.data[ii]));
        }
    }
    os << std::dec << '>';
    return os;
}

bool Item::compressValue(float minCompressionRatio) {
    auto datatype = getDataType();
    if (!mcbp::datatype::is_snappy(datatype)) {
        // Attempt compression only if datatype indicates
        // that the value is not compressed already.
        cb::compression::Buffer deflated;
        if (cb::compression::deflate(cb::compression::Algorithm::Snappy,
                                     getData(), getNBytes(), deflated)) {
            if (deflated.len > minCompressionRatio * getNBytes()) {
                // No point doing the compression if the desired
                // compression ratio isn't achieved.
                return true;
            }
            setData(deflated.data.get(), deflated.len,
                    (uint8_t *)(getExtMeta()), getExtMetaLen());

            datatype |= PROTOCOL_BINARY_DATATYPE_SNAPPY;
            setDataType(datatype);
        } else {
            return false;
        }
    }
    return true;
}

bool Item::decompressValue() {
    uint8_t datatype = getDataType();
    if (mcbp::datatype::is_snappy(datatype)) {
        // Attempt decompression only if datatype indicates
        // that the value is compressed.
        cb::compression::Buffer inflated;
        if (cb::compression::inflate(cb::compression::Algorithm::Snappy,
                                     getData(), getNBytes(), inflated)) {
            setData(inflated.data.get(), inflated.len,
                    (uint8_t *)(getExtMeta()), getExtMetaLen());
            datatype &= ~PROTOCOL_BINARY_DATATYPE_SNAPPY;
            setDataType(datatype);
        } else {
            return false;
        }
    }
    return true;
}

item_info Item::toItemInfo(uint64_t vb_uuid) const {
    item_info info;
    info.cas = getCas();
    info.vbucket_uuid = vb_uuid;
    info.seqno = getBySeqno();
    info.exptime = getExptime();
    info.nbytes = getNBytes();
    info.flags = getFlags();
    info.datatype = getDataType();

    if (isDeleted()) {
        info.document_state = DocumentState::Deleted;
    } else {
        info.document_state = DocumentState::Alive;
    }
    info.nkey = static_cast<uint16_t>(getKey().size());
    info.key = getKey().data();
    info.value[0].iov_base = const_cast<char*>(getData());
    info.value[0].iov_len = getNBytes();

    return info;
}
