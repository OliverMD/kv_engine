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

#include "hash_table.h"

#include "item.h"
#include "stats.h"
#include "stored_value_factories.h"

#include <cstring>

static const ssize_t prime_size_table[] = {
    3, 7, 13, 23, 47, 97, 193, 383, 769, 1531, 3079, 6143, 12289, 24571, 49157,
    98299, 196613, 393209, 786433, 1572869, 3145721, 6291449, 12582917,
    25165813, 50331653, 100663291, 201326611, 402653189, 805306357,
    1610612741, -1
};


std::ostream& operator<<(std::ostream& os, const HashTable::Position& pos) {
    os << "{lock:" << pos.lock << " bucket:" << pos.hash_bucket << "/" << pos.ht_size << "}";
    return os;
}

HashTable::HashTable(EPStats& st,
                     std::unique_ptr<AbstractStoredValueFactory> svFactory,
                     size_t initialSize,
                     size_t locks)
    : maxDeletedRevSeqno(0),
      numTotalItems(0),
      numNonResidentItems(0),
      numDeletedItems(0),
      datatypeCounts(),
      numEjects(0),
      memSize(0),
      cacheSize(0),
      metaDataMemory(0),
      initialSize(initialSize),
      size(initialSize),
      n_locks(locks),
      stats(st),
      valFact(std::move(svFactory)),
      visitors(0),
      numItems(0),
      numResizes(0),
      numTempItems(0) {
    values.resize(size);
    mutexes = new std::mutex[n_locks];
    activeState = true;
}

HashTable::~HashTable() {
    // Use unlocked clear for the destructor, avoids lock inversions on VBucket
    // delete
    clear_UNLOCKED(true);
    // Wait for any outstanding visitors to finish.
    while (visitors > 0) {
#ifdef _MSC_VER
        Sleep(1);
#else
        usleep(100);
#endif
    }
    delete []mutexes;
}

void HashTable::clear(bool deactivate) {
    if (!deactivate) {
        // If not deactivating, assert we're already active.
        if (!isActive()) {
            throw std::logic_error("HashTable::clear: Cannot call on a "
                    "non-active object");
        }
    }
    MultiLockHolder mlh(mutexes, n_locks);
    clear_UNLOCKED(deactivate);
}

void HashTable::clear_UNLOCKED(bool deactivate) {
    if (deactivate) {
        setActiveState(false);
    }
    size_t clearedMemSize = 0;
    size_t clearedValSize = 0;
    for (int i = 0; i < (int)size; i++) {
        while (values[i]) {
            // Take ownership of the StoredValue from the vector, update
            // statistics and release it.
            auto v = std::move(values[i]);
            clearedMemSize += v->size();
            clearedValSize += v->valuelen();
            values[i] = std::move(v->getNext());
        }
    }

    stats.currentSize.fetch_sub(clearedMemSize - clearedValSize);

    datatypeCounts.fill(0);
    numTotalItems.store(0);
    numItems.store(0);
    numTempItems.store(0);
    numNonResidentItems.store(0);
    memSize.store(0);
    cacheSize.store(0);
}

static size_t distance(size_t a, size_t b) {
    return std::max(a, b) - std::min(a, b);
}

static size_t nearest(size_t n, size_t a, size_t b) {
    return (distance(n, a) < distance(b, n)) ? a : b;
}

static bool isCurrently(size_t size, ssize_t a, ssize_t b) {
    ssize_t current(static_cast<ssize_t>(size));
    return (current == a || current == b);
}

void HashTable::resize() {
    size_t ni = getNumInMemoryItems();
    int i(0);
    size_t new_size(0);

    // Figure out where in the prime table we are.
    ssize_t target(static_cast<ssize_t>(ni));
    for (i = 0; prime_size_table[i] > 0 && prime_size_table[i] < target; ++i) {
        // Just looking...
    }

    if (prime_size_table[i] == -1) {
        // We're at the end, take the biggest
        new_size = prime_size_table[i-1];
    } else if (prime_size_table[i] < static_cast<ssize_t>(initialSize)) {
        // Was going to be smaller than the initial size.
        new_size = initialSize;
    } else if (0 == i) {
        new_size = prime_size_table[i];
    }else if (isCurrently(size, prime_size_table[i-1], prime_size_table[i])) {
        // If one of the candidate sizes is the current size, maintain
        // the current size in order to remain stable.
        new_size = size;
    } else {
        // Somewhere in the middle, use the one we're closer to.
        new_size = nearest(ni, prime_size_table[i-1], prime_size_table[i]);
    }

    resize(new_size);
}

void HashTable::resize(size_t newSize) {
    if (!isActive()) {
        throw std::logic_error("HashTable::resize: Cannot call on a "
                "non-active object");
    }

    // Due to the way hashing works, we can't fit anything larger than
    // an int.
    if (newSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }

    // Don't resize to the same size, either.
    if (newSize == size) {
        return;
    }

    MultiLockHolder mlh(mutexes, n_locks);
    if (visitors.load() > 0) {
        // Do not allow a resize while any visitors are actually
        // processing.  The next attempt will have to pick it up.  New
        // visitors cannot start doing meaningful work (we own all
        // locks at this point).
        return;
    }

    // Get a place for the new items.
    table_type newValues(newSize);

    stats.memOverhead->fetch_sub(memorySize());
    ++numResizes;

    // Set the new size so all the hashy stuff works.
    size_t oldSize = size;
    size.store(newSize);

    // Move existing records into the new space.
    for (size_t i = 0; i < oldSize; i++) {
        while (values[i]) {
            // unlink the front element from the hash chain at values[i].
            auto v = std::move(values[i]);
            values[i] = std::move(v->getNext());

            // And re-link it into the correct place in newValues.
            int newBucket = getBucketForHash(v->getKey().hash());
            v->setNext(std::move(newValues[newBucket]));
            newValues[newBucket] = std::move(v);
        }
    }

    // Finally assign the new table to values.
    values = std::move(newValues);

    stats.memOverhead->fetch_add(memorySize());
}

StoredValue* HashTable::find(const DocKey& key,
                             TrackReference trackReference,
                             WantsDeleted wantsDeleted) {
    if (!isActive()) {
        throw std::logic_error("HashTable::find: Cannot call on a "
                "non-active object");
    }
    HashBucketLock hbl = getLockedBucket(key);
    return unlocked_find(key, hbl.getBucketNum(), wantsDeleted, trackReference);
}

std::unique_ptr<Item> HashTable::getRandomKey(long rnd) {
    /* Try to locate a partition */
    size_t start = rnd % size;
    size_t curr = start;
    std::unique_ptr<Item> ret;

    do {
        ret = getRandomKeyFromSlot(curr++);
        if (curr == size) {
            curr = 0;
        }
    } while (ret == NULL && curr != start);

    return ret;
}

MutationStatus HashTable::set(Item& val) {
    if (!StoredValue::hasAvailableSpace(stats, val, false)) {
        return MutationStatus::NoMem;
    }

    HashBucketLock hbl = getLockedBucket(val.getKey());
    StoredValue* v = unlocked_find(val.getKey(),
                                   hbl.getBucketNum(),
                                   WantsDeleted::Yes,
                                   TrackReference::No);
    if (v) {
        return unlocked_updateStoredValue(hbl.getHTLock(), *v, val);
    } else {
        unlocked_addNewStoredValue(hbl, val);
        return MutationStatus::WasClean;
    }
}

MutationStatus HashTable::unlocked_updateStoredValue(
        const std::unique_lock<std::mutex>& htLock,
        StoredValue& v,
        const Item& itm) {
    if (!htLock) {
        throw std::invalid_argument(
                "HashTable::unlocked_updateStoredValue: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_updateStoredValue: Cannot "
                "call on a non-active HT object");
    }

    MutationStatus status =
            v.isDirty() ? MutationStatus::WasDirty : MutationStatus::WasClean;
    if (!v.isResident() && !v.isDeleted() && !v.isTempItem()) {
        decrNumNonResidentItems();
    }

    if (v.isTempItem()) {
        --numTempItems;
        ++numItems;
        ++numTotalItems;
    }

    if (v.isDeleted() && !itm.isDeleted()) {
        --numDeletedItems;
    }
    if (!v.isDeleted() && itm.isDeleted()) {
        ++numDeletedItems;
    }

    // If the item we are replacing is resident then we need to make sure we
    // appropriately alter the datatype stats.
    if (v.getDatatype() != itm.getDataType()) {
        --datatypeCounts[v.getDatatype()];
        ++datatypeCounts[itm.getDataType()];
    }

    /* setValue() will mark v as undeleted if required */
    setValue(itm, v);

    return status;
}

StoredValue* HashTable::unlocked_addNewStoredValue(const HashBucketLock& hbl,
                                                   const Item& itm) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_addNewStoredValue: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::invalid_argument(
                "HashTable::unlocked_addNewStoredValue: Cannot "
                "call on a non-active HT object");
    }

    // Create a new StoredValue and link it into the head of the bucket chain.
    auto v = (*valFact)(itm, std::move(values[hbl.getBucketNum()]));
    increaseMetaDataSize(stats, v->metaDataSize());
    increaseCacheSize(v->size());

    if (v->isTempItem()) {
        ++numTempItems;
    } else {
        ++numItems;
        ++numTotalItems;
        ++datatypeCounts[v->getDatatype()];
    }
    if (v->isDeleted()) {
        ++numDeletedItems;
    }
    values[hbl.getBucketNum()] = std::move(v);

    return values[hbl.getBucketNum()].get();
}

std::pair<StoredValue*, StoredValue::UniquePtr>
HashTable::unlocked_replaceByCopy(const HashBucketLock& hbl,
                                  const StoredValue& vToCopy) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_replaceByCopy: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::invalid_argument(
                "HashTable::unlocked_replaceByCopy: Cannot "
                "call on a non-active HT object");
    }

    /* Release (remove) the StoredValue from the hash table */
    auto releasedSv = unlocked_release(hbl, vToCopy.getKey());

    /* Copy the StoredValue and link it into the head of the bucket chain. */
    auto newSv = valFact->copyStoredValue(
            vToCopy, std::move(values[hbl.getBucketNum()]));
    if (newSv->isTempItem()) {
        ++numTempItems;
    } else {
        ++numItems;
        ++numTotalItems;
    }
    values[hbl.getBucketNum()] = std::move(newSv);

    return {values[hbl.getBucketNum()].get(), std::move(releasedSv)};
}

void HashTable::unlocked_softDelete(const std::unique_lock<std::mutex>& htLock,
                                    StoredValue& v,
                                    bool onlyMarkDeleted) {
    const bool alreadyDeleted = v.isDeleted();
    if (!v.isResident() && !v.isDeleted() && !v.isTempItem()) {
        decrNumNonResidentItems();
    }

    --datatypeCounts[v.getDatatype()];

    if (onlyMarkDeleted) {
        v.markDeleted();
    } else {
        if (v.isTempItem()) {
            --numTempItems;
            ++numItems;
            ++numTotalItems;
        }
        size_t len = v.valuelen();
        if (v.del()) {
            reduceCacheSize(len);
        }
    }
    if (!alreadyDeleted) {
        ++numDeletedItems;
    }
}

StoredValue* HashTable::unlocked_find(const DocKey& key,
                                      int bucket_num,
                                      WantsDeleted wantsDeleted,
                                      TrackReference trackReference) {
    for (StoredValue* v = values[bucket_num].get(); v; v = v->getNext().get()) {
        if (v->hasKey(key)) {
            if (trackReference == TrackReference::Yes && !v->isDeleted()) {
                v->referenced();
            }
            if (wantsDeleted == WantsDeleted::Yes || !v->isDeleted()) {
                return v;
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

void HashTable::unlocked_del(const HashBucketLock& hbl, const DocKey& key) {
    unlocked_release(hbl, key).reset();
}

StoredValue::UniquePtr HashTable::unlocked_release(
        const HashBucketLock& hbl, const DocKey& key) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_remove: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_remove: Cannot call on a "
                "non-active object");
    }

    // Remove the first (should only be one) StoredValue with the given key.
    auto released = hashChainRemoveFirst(
            values[hbl.getBucketNum()],
            [key](const StoredValue* v) { return v->hasKey(key); });

    if (!released) {
        /* We shouldn't reach here, we must delete the StoredValue in the
           HashTable */
        throw std::logic_error(
                "HashTable::unlocked_del: StoredValue to be deleted "
                "not found in HashTable; possibly HashTable leak");
    }

    // Update statistics now the item has been removed.
    reduceCacheSize(released->size());
    reduceMetaDataSize(stats, released->metaDataSize());
    if (released->isTempItem()) {
        --numTempItems;
    } else {
        decrNumItems();
        decrNumTotalItems();
        --datatypeCounts[released->getDatatype()];
        if (released->isDeleted()) {
            --numDeletedItems;
        }
    }
    return released;
}

void HashTable::visit(HashTableVisitor &visitor) {
    if ((numItems.load() + numTempItems.load()) == 0 || !isActive()) {
        return;
    }

    // Acquire one (any) of the mutexes before incrementing {visitors}, this
    // prevents any race between this visitor and the HashTable resizer.
    // See comments in pauseResumeVisit() for further details.
    std::unique_lock<std::mutex> lh(mutexes[0]);
    VisitorTracker vt(&visitors);
    lh.unlock();

    size_t visited = 0;
    for (int l = 0; isActive() && l < static_cast<int>(n_locks); l++) {
        for (int i = l; i < static_cast<int>(size); i+= n_locks) {
            // (re)acquire mutex on each HashBucket, to minimise any impact
            // on front-end threads.
            HashBucketLock lh(i, mutexes[l]);

            StoredValue* v = values[i].get();
            if (v) {
                // TODO: Perf: This check seems costly - do we think it's still
                // worth keeping?
                auto hashbucket = getBucketForHash(v->getKey().hash());
                if (i != hashbucket) {
                    throw std::logic_error("HashTable::visit: inconsistency "
                            "between StoredValue's calculated hashbucket "
                            "(which is " + std::to_string(hashbucket) +
                            ") and bucket is is located in (which is " +
                            std::to_string(i) + ")");
                }
            }
            while (v) {
                StoredValue* tmp = v->getNext().get();
                visitor.visit(lh, *v);
                v = tmp;
            }
            ++visited;
        }
    }
}

void HashTable::visitDepth(HashTableDepthVisitor &visitor) {
    if (numItems.load() == 0 || !isActive()) {
        return;
    }
    size_t visited = 0;
    VisitorTracker vt(&visitors);

    for (int l = 0; l < static_cast<int>(n_locks); l++) {
        LockHolder lh(mutexes[l]);
        for (int i = l; i < static_cast<int>(size); i+= n_locks) {
            size_t depth = 0;
            StoredValue* p = values[i].get();
            if (p) {
                // TODO: Perf: This check seems costly - do we think it's still
                // worth keeping?
                auto hashbucket = getBucketForHash(p->getKey().hash());
                if (i != hashbucket) {
                    throw std::logic_error("HashTable::visit: inconsistency "
                            "between StoredValue's calculated hashbucket "
                            "(which is " + std::to_string(hashbucket) +
                            ") and bucket it is located in (which is " +
                            std::to_string(i) + ")");
                }
            }
            size_t mem(0);
            while (p) {
                depth++;
                mem += p->size();
                p = p->getNext().get();
            }
            visitor.visit(i, depth, mem);
            ++visited;
        }
    }
}

HashTable::Position HashTable::pauseResumeVisit(HashTableVisitor& visitor,
                                                Position& start_pos) {
    if ((numItems.load() + numTempItems.load()) == 0 || !isActive()) {
        // Nothing to visit
        return endPosition();
    }

    bool paused = false;

    // To attempt to minimize the impact the visitor has on normal frontend
    // operations, we deliberately acquire (and release) the mutex between
    // each hash_bucket - see `lh` in the inner for() loop below. This means we
    // hold a given mutex for a large number of short durations, instead of just
    // one single, long duration.
    // *However*, there is a potential race with this approach - the {size} of
    // the HashTable may be changed (by the Resizer task) between us first
    // reading it to calculate the starting hash_bucket, and then reading it
    // inside the inner for() loop. To prevent this race, we explicitly acquire
    // (any) mutex, increment {visitors} and then release the mutex. This
    //avoids the race as if visitors >0 then Resizer will not attempt to resize.
    std::unique_lock<std::mutex> lh(mutexes[0]);
    VisitorTracker vt(&visitors);
    lh.unlock();

    // Start from the requested lock number if in range.
    size_t lock = (start_pos.lock < n_locks) ? start_pos.lock : 0;
    size_t hash_bucket = 0;

    for (; isActive() && !paused && lock < n_locks; lock++) {

        // If the bucket position is *this* lock, then start from the
        // recorded bucket (as long as we haven't resized).
        hash_bucket = lock;
        if (start_pos.lock == lock &&
            start_pos.ht_size == size &&
            start_pos.hash_bucket < size) {
            hash_bucket = start_pos.hash_bucket;
        }

        // Iterate across all values in the hash buckets owned by this lock.
        // Note: we don't record how far into the bucket linked-list we
        // pause at; so any restart will begin from the next bucket.
        for (; !paused && hash_bucket < size; hash_bucket += n_locks) {
            HashBucketLock lh(lock, mutexes[lock]);

            StoredValue* v = values[hash_bucket].get();
            while (!paused && v) {
                StoredValue* tmp = v->getNext().get();
                paused = !visitor.visit(lh, *v);
                v = tmp;
            }
        }

        // If the visitor paused us before we visited all hash buckets owned
        // by this lock, we don't want to skip the remaining hash buckets, so
        // stop the outer for loop from advancing to the next lock.
        if (paused && hash_bucket < size) {
            break;
        }

        // Finished all buckets owned by this lock. Set hash_bucket to 'size'
        // to give a consistent marker for "end of lock".
        hash_bucket = size;
    }

    // Return the *next* location that should be visited.
    return HashTable::Position(size, lock, hash_bucket);
}

HashTable::Position HashTable::endPosition() const  {
    return HashTable::Position(size, n_locks, size);
}

bool HashTable::unlocked_ejectItem(StoredValue*& vptr,
                                   item_eviction_policy_t policy) {
    if (vptr == nullptr) {
        throw std::invalid_argument("HashTable::unlocked_ejectItem: "
                "Unable to delete NULL StoredValue");
    }
    if (policy == VALUE_ONLY) {
        if (vptr->eligibleForEviction(policy)) {
            reduceCacheSize(vptr->valuelen());
            vptr->ejectValue();
            ++stats.numValueEjects;
            ++numNonResidentItems;
            ++numEjects;
            return true;
        } else {
            ++stats.numFailedEjects;
            return false;
        }
    } else { // full eviction.
        if (vptr->eligibleForEviction(policy)) {
            reduceMetaDataSize(stats, vptr->metaDataSize());
            reduceCacheSize(vptr->size());
            int bucket_num = getBucketForHash(vptr->getKey().hash());

            // Remove the item from the hash table.
            auto removed = hashChainRemoveFirst(
                    values[bucket_num],
                    [vptr](const StoredValue* v) { return v == vptr; });

            if (removed->isResident()) {
                ++stats.numValueEjects;
            }
            if (!removed->isResident() && !removed->isTempItem()) {
                decrNumNonResidentItems(); // Decrement because the item is
                                           // fully evicted.
            }
            decrNumItems(); // Decrement because the item is fully evicted.
            --datatypeCounts[vptr->getDatatype()];
            ++numEjects;
            updateMaxDeletedRevSeqno(vptr->getRevSeqno());

            return true;
        } else {
            ++stats.numFailedEjects;
            return false;
        }
    }
}

std::unique_ptr<Item> HashTable::getRandomKeyFromSlot(int slot) {
    auto lh = getLockedBucket(slot);
    for (StoredValue* v = values[slot].get(); v; v = v->getNext().get()) {
        if (!v->isTempItem() && !v->isDeleted() && v->isResident()) {
            return v->toItem(false, 0);
        }
    }

    return nullptr;
}

bool HashTable::unlocked_restoreValue(
        const std::unique_lock<std::mutex>& htLock,
        const Item& itm,
        StoredValue& v) {
    if (!htLock || !isActive() || v.isResident()) {
        return false;
    }

    if (v.isTempInitialItem()) { // Regular item with the full eviction
        --numTempItems;
        ++numItems;
        /* set it back to false as we created a temp item by setting it to true
           when bg fetch is scheduled (full eviction mode). */
        v.setNewCacheItem(false);
        ++datatypeCounts[itm.getDataType()];
    } else {
        decrNumNonResidentItems();
    }

    v.restoreValue(itm);

    increaseCacheSize(v.getValue()->length());
    return true;
}

void HashTable::unlocked_restoreMeta(const std::unique_lock<std::mutex>& htLock,
                                     const Item& itm,
                                     StoredValue& v) {
    if (!htLock) {
        throw std::invalid_argument(
                "HashTable::unlocked_restoreMeta: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_restoreMeta: Cannot "
                "call on a non-active HT object");
    }

    v.restoreMeta(itm);
    if (!itm.isDeleted()) {
        --numTempItems;
        ++numItems;
        ++numNonResidentItems;
        ++datatypeCounts[v.getDatatype()];
    }
}

void HashTable::increaseCacheSize(size_t by) {
    cacheSize.fetch_add(by);
    memSize.fetch_add(by);
}

void HashTable::reduceCacheSize(size_t by) {
    cacheSize.fetch_sub(by);
    memSize.fetch_sub(by);
}

void HashTable::increaseMetaDataSize(EPStats& st, size_t by) {
    metaDataMemory.fetch_add(by);
    st.currentSize.fetch_add(by);
}

void HashTable::reduceMetaDataSize(EPStats &st, size_t by) {
    metaDataMemory.fetch_sub(by);
    st.currentSize.fetch_sub(by);
}

void HashTable::setValue(const Item& itm, StoredValue& v) {
    reduceCacheSize(v.size());
    v.setValue(itm);
    increaseCacheSize(v.size());
}

std::ostream& operator<<(std::ostream& os, const HashTable& ht) {
    os << "HashTable[" << &ht << "] with"
       << " numInMemory:" << ht.getNumInMemoryItems()
       << " numDeleted:" << ht.getNumDeletedItems()
       << " values: " << std::endl;
    for (const auto& chain : ht.values) {
        if (chain) {
            for (StoredValue* sv = chain.get(); sv != nullptr;
                 sv = sv->getNext().get()) {
                os << "    " << *sv << std::endl;
            }
        }
    }
    return os;
}
