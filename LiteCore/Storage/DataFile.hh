//
//  DataFile.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once
#include "KeyStore.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "RefCounted.hh"
#include <vector>
#include <unordered_map>
#include <atomic> // for std::atomic_uint
#include <functional> // for std::function
#ifdef check
#undef check
#endif

namespace fleece {
    class SharedKeys;
    class PersistentSharedKeys;
}

namespace litecore {

    class Transaction;


    /** A database file, primarily a container of KeyStores which store the actual data.
        This is an abstract class, with concrete subclasses for different database engines. */
    class DataFile {
    public:

        // Callback that takes a record body and returns the portion of it containing Fleece data
        typedef slice (*FleeceAccessor)(slice recordBody);

        struct Options {
            KeyStore::Capabilities keyStores;
            bool                create         :1;      ///< Should the db be created if it doesn't exist?
            bool                writeable      :1;      ///< If false, db is opened read-only
            bool                useDocumentKeys:1;      ///< Use SharedKeys for Fleece docs
            EncryptionAlgorithm encryptionAlgorithm;    ///< What encryption (if any)
            alloc_slice         encryptionKey;          ///< Encryption key, if encrypting
            FleeceAccessor      fleeceAccessor;         ///< Fn to get Fleece from Record body

            static const Options defaults;
        };

        DataFile(const FilePath &path, const Options* =nullptr);
        virtual ~DataFile();

        const FilePath& filePath() const noexcept;
        const Options& options() const noexcept              {return _options;}

        virtual bool isOpen() const noexcept =0;

        /** Throws an exception if the database is closed. */
        void checkOpen() const;

        /** Closes the database. Do not call any methods on this object afterwards,
            except isOpen() or mustBeOpen(), before deleting it. */
        virtual void close();

        /** Closes the database and deletes its file. */
        virtual void deleteDataFile() =0;

        virtual void compact() =0;

        virtual void rekey(EncryptionAlgorithm, slice newKey);

        FleeceAccessor fleeceAccessor() const               {return _options.fleeceAccessor;}
        fleece::SharedKeys* documentKeys() const;

        void* owner()                                       {return _owner;}
        void setOwner(void* owner)                          {_owner = owner;}

        void forOtherDataFiles(function_ref<void(DataFile*)> fn);

        /** Private API to run a raw (e.g. SQL) query, for diagnostic purposes only */
        virtual fleece::alloc_slice rawQuery(const std::string &query) =0;

        //////// KEY-STORES:

        static const std::string kDefaultKeyStoreName;
        static const std::string kInfoKeyStoreName;

        /** The DataFile's default key-value store. */
        KeyStore& defaultKeyStore() const           {return defaultKeyStore(_options.keyStores);}
        KeyStore& defaultKeyStore(KeyStore::Capabilities) const;

        KeyStore& getKeyStore(const std::string &name) const;
        KeyStore& getKeyStore(const std::string &name, KeyStore::Capabilities) const;

        /** The names of all existing KeyStores (whether opened yet or not) */
        virtual std::vector<std::string> allKeyStoreNames() =0;

        void closeKeyStore(const std::string &name);

#if ENABLE_DELETE_KEY_STORES
        /** Permanently deletes a KeyStore. */
        virtual void deleteKeyStore(const std::string &name) =0;
#endif


        //////// SHARED OBJECTS:

        Retained<RefCounted> sharedObject(const std::string &key);
        Retained<RefCounted> addSharedObject(const std::string &key, Retained<RefCounted>);


        //////// FACTORY:

        /** Abstract factory for creating/managing DataFiles. */
        class Factory {
        public:
            std::string name()  {return std::string(cname());}
            virtual const char* cname() =0;
            virtual std::string filenameExtension() =0;
            virtual bool encryptionEnabled(EncryptionAlgorithm) =0;

            /** The number of currently open DataFiles on the given path. */
            size_t openCount(const FilePath &path);

            /** Opens a DataFile. */
            virtual DataFile* openFile(const FilePath &path, const Options* =nullptr) =0;

            /** Deletes a non-open file. Returns false if it doesn't exist. */
            virtual bool deleteFile(const FilePath &path, const Options* =nullptr);

            /** Moves a non-open file. */
            virtual void moveFile(const FilePath &fromPath, const FilePath &toPath);

            /** Does a file exist at this path? */
            virtual bool fileExists(const FilePath &path);
            
        protected:
            virtual ~Factory() { }
        };

        static std::vector<Factory*> factories();
        static Factory* factoryNamed(const std::string &name);
        static Factory* factoryNamed(const char *name);
        static Factory* factoryForFile(const FilePath&);

    protected:
        /** Reopens database after it's been closed. */
        virtual void reopen();

        /** Override to instantiate a KeyStore object. */
        virtual KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) =0;

        /** Override to begin a database transaction. */
        virtual void _beginTransaction(Transaction* t NONNULL) =0;

        /** Override to commit or abort a database transaction. */
        virtual void _endTransaction(Transaction* t NONNULL, bool commit) =0;

        /** Is this DataFile object currently in a transaction? */
        bool inTransaction() const                      {return _inTransaction;}

        /** Override to begin a read-only transaction. */
        virtual void beginReadOnlyTransaction() =0;

        /** Override to end a read-only transaction. */
        virtual void endReadOnlyTransaction() =0;

        /** Runs the function/lambda while holding the file lock. This doesn't create a real
            transaction (at the ForestDB/SQLite/etc level), but it does ensure that no other thread
            is in a transaction, nor starts a transaction while the function is running. */
        void withFileLock(function_ref<void(void)> fn);

        void setOptions(const Options &o)               {_options = o;}

        void forOpenKeyStores(function_ref<void(KeyStore&)> fn);

    private:
        class Shared;
        friend class KeyStore;
        friend class Transaction;
        friend class ReadOnlyTransaction;
        friend class DocumentKeys;

        KeyStore& addKeyStore(const std::string &name, KeyStore::Capabilities);
        void beginTransactionScope(Transaction*);
        void transactionBegan(Transaction*);
        void transactionEnding(Transaction*, bool committing);
        void endTransactionScope(Transaction*);
        Transaction& transaction();

        DataFile(const DataFile&) = delete;
        DataFile& operator=(const DataFile&) = delete;

        Retained<Shared>        _shared;                        // Shared state of file (lock)
        Options                 _options;                       // Option/capability flags
        KeyStore*               _defaultKeyStore {nullptr};     // The default KeyStore
        std::unordered_map<std::string, std::unique_ptr<KeyStore>> _keyStores;// Opened KeyStores
        std::unique_ptr<fleece::PersistentSharedKeys> _documentKeys;
        bool                    _inTransaction {false};         // Am I in a Transaction?
        std::atomic<void*>      _owner {nullptr};               // App-defined object that owns me
    };


    /** Grants exclusive write access to a DataFile while in scope.
        The transaction is committed when the object exits scope, unless abort() was called.
        Only one Transaction object can be created on a database file at a time.
        Not just per DataFile object; per database _file_. */
    class Transaction {
    public:
        explicit Transaction(DataFile*);
        explicit Transaction(DataFile &db)  :Transaction(&db) { }
        ~Transaction();

        DataFile& dataFile() const          {return _db;}

        void commit();
        void abort();

    private:
        friend class DataFile;
        friend class KeyStore;

        Transaction(DataFile*, bool begin);
        Transaction(const Transaction&) = delete;

        DataFile&   _db;        // The DataFile
        bool _active;           // Is there an open transaction at the db level?
    };


    /** A read-only transaction. Does not grant access to writes, but ensures that all database
        reads are consistent with each other.
        Multiple DataFile instances on the same file may have simultaneous ReadOnlyTransactions,
        and they can coexist with a simultaneous Transaction (but will be isolated from its
        changes.) */
    class ReadOnlyTransaction {
    public:
        explicit ReadOnlyTransaction(DataFile *db);
        explicit ReadOnlyTransaction(DataFile &db)  :ReadOnlyTransaction(&db) { }
        ~ReadOnlyTransaction();
    private:
        ReadOnlyTransaction(const ReadOnlyTransaction&) = delete;

        DataFile *_db {nullptr};
    };

}
