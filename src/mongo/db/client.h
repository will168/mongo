/* @file db/client.h

   "Client" represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.

   todo: switch to asio...this will fit nicely with that.
*/

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/pch.h"

#include "mongo/db/client_basic.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/lockstate.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/paths.h"

namespace mongo {

    extern class ReplSet *theReplSet;
    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Command;
    class Client;
    class AbstractMessagingPort;
    class LockCollectionForReading;
    class PageFaultRetryableSection;

    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

    /** the database's concept of an outside "client" */
    class Client : public ClientBasic {
    public:
        // always be in clientsMutex when manipulating this. killop stuff uses these.
        static set<Client*>& clients;
        static mongo::mutex& clientsMutex;
        static int getActiveClientCount( int& writers , int& readers );
        class Context;
        ~Client();
        static int recommendedYieldMicros( int * writers = 0 , int * readers = 0,
                                           bool needExact = false );
        /** each thread which does db operations has a Client object in TLS.
         *  call this when your thread starts.
        */
        static Client& initThread(const char *desc, AbstractMessagingPort *mp = 0);

        static void initThreadIfNotAlready(const char *desc) { 
            if( currentClient.get() )
                return;
            initThread(desc);
        }

        /** this has to be called as the client goes away, but before thread termination
         *  @return true if anything was done
         */
        bool shutdown();

        string clientAddress(bool includePort=false) const;
        CurOp* curop() const { return _curOp; }
        Context* getContext() const { return _context; }
        Database* database() const {  return _context ? _context->db() : 0; }
        const StringData desc() const { return _desc; }
        void setLastOp( OpTime op ) { _lastOp = op; }
        OpTime getLastOp() const { return _lastOp; }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) const;

        bool isGod() const { return _god; } /* this is for map/reduce writes */
        bool setGod(bool newVal) { const bool prev = _god; _god = newVal; return prev; }
        string toString() const;
        void gotHandshake( const BSONObj& o );
        BSONObj getRemoteID() const { return _remoteId; }
        BSONObj getHandshake() const { return _handshake; }
        ConnectionId getConnectionId() const { return _connectionId; }

        bool inPageFaultRetryableSection() const { return _pageFaultRetryableSection != 0; }
        PageFaultRetryableSection* getPageFaultRetryableSection() const { return _pageFaultRetryableSection; }

        void writeHappened() { _hasWrittenSinceCheckpoint = true; _hasWrittenThisOperation = true; }
        bool hasWrittenSinceCheckpoint() const { return _hasWrittenSinceCheckpoint; }
        void checkpointHappened() { _hasWrittenSinceCheckpoint = false; }
        bool hasWrittenThisOperation() const { return _hasWrittenThisOperation; }
        void newTopLevelRequest() {
            _hasWrittenThisOperation = false;
            _hasWrittenSinceCheckpoint = false;
        }

        /**
         * Call this to allow PageFaultExceptions even if writes happened before this was called.
         * Writes after this is called still prevent PFEs from being thrown.
         */
        void clearHasWrittenThisOperation() { _hasWrittenThisOperation = false; }

        bool allowedToThrowPageFaultException() const;

        LockState& lockState() { return _ls; }

    private:
        Client(const std::string& desc, AbstractMessagingPort *p = 0);
        friend class CurOp;
        ConnectionId _connectionId; // > 0 for things "conn", 0 otherwise
        string _threadId; // "" on non support systems
        CurOp * _curOp;
        Context * _context;
        bool _shutdown; // to track if Client::shutdown() gets called
        std::string _desc;
        bool _god;
        OpTime _lastOp;
        BSONObj _handshake;
        BSONObj _remoteId;

        bool _hasWrittenThisOperation;
        bool _hasWrittenSinceCheckpoint;
        PageFaultRetryableSection *_pageFaultRetryableSection;

        LockState _ls;
        
        friend class PageFaultRetryableSection; // TEMP
        friend class NoPageFaultsAllowed; // TEMP
    public:

        /** "read lock, and set my context, all in one operation" 
         *  This handles (if not recursively locked) opening an unopened database.
         */
        class ReadContext : boost::noncopyable { 
        public:
            ReadContext(const std::string& ns, const std::string& path=storageGlobalParams.dbpath);
            Context& ctx() { return *c.get(); }
        private:
            scoped_ptr<Lock::DBRead> lk;
            scoped_ptr<Context> c;
        };

        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context : boost::noncopyable {
        public:
            /** this is probably what you want */
            Context(const string& ns, const std::string& path=storageGlobalParams.dbpath,
                    bool doVersion=true);

            /** note: this does not call finishInit -- i.e., does not call 
                      shardVersionOk() for example. 
                see also: reset().
            */
            Context(const std::string& ns , Database * db);

            // used by ReadContext
            Context(const string& path, const string& ns, Database *db);

            ~Context();
            Client* getClient() const { return _client; }
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }
            bool equals(const string& ns, const string& path=storageGlobalParams.dbpath) const {
                return _ns == ns && _path == path;
            }

            /** @return if the db was created by this Context */
            bool justCreated() const { return _justCreated; }

            /** @return true iff the current Context is using db/path */
            bool inDB(const string& db, const string& path=storageGlobalParams.dbpath) const;

            void _clear() { // this is sort of an "early destruct" indication, _ns can never be uncleared
                const_cast<string&>(_ns).clear();
                _db = 0;
            }

            /** call before unlocking, so clear any non-thread safe state
             *  _db gets restored on the relock
             */
            void unlocked() { _db = 0; }

            /** call after going back into the lock, will re-establish non-thread safe stuff */
            void relocked() { _finishInit(); }

        private:
            friend class CurOp;
            void _finishInit();
            void checkNotStale() const;
            void checkNsAccess( bool doauth );
            void checkNsAccess( bool doauth, int lockState );
            Client * const _client;
            Context * const _oldContext;
            const string _path;
            bool _justCreated;
            bool _doVersion;
            const string _ns;
            Database * _db;
            
            Timer _timer;
        }; // class Client::Context

        class WriteContext : boost::noncopyable {
        public:
            WriteContext(const string& ns, const std::string& path=storageGlobalParams.dbpath);
            Context& ctx() { return _c; }
        private:
            Lock::DBWrite _lk;
            Context _c;
        };


    }; // class Client


    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        verify( c );
        return *c;
    }

    inline bool haveClient() { return currentClient.get() > 0; }

};
