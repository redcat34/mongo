// list_collections.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

    class CmdListCollections : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual void help( stringstream& help ) const { help << "list collections for this db"; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::listCollections);
            out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
        }

        CmdListCollections() : Command( "listCollections", true ) {}

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& jsobj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool /*fromRepl*/) {

            ScopedTransaction scopedXact(txn, MODE_IS);
            AutoGetDb autoDb(txn, dbname, MODE_S);

            const Database* d = autoDb.getDb();
            const DatabaseCatalogEntry* dbEntry = NULL;

            list<string> names;
            if ( d ) {
                dbEntry = d->getDatabaseCatalogEntry();
                dbEntry->getCollectionNamespaces( &names );
                names.sort();
            }

            scoped_ptr<MatchExpression> matcher;
            if ( jsobj["filter"].isABSONObj() ) {
                StatusWithMatchExpression parsed =
                    MatchExpressionParser::parse( jsobj["filter"].Obj() );
                if ( !parsed.isOK() ) {
                    return appendCommandStatus( result, parsed.getStatus() );
                }
                matcher.reset( parsed.getValue() );
            }

            std::auto_ptr<WorkingSet> ws(new WorkingSet());
            std::auto_ptr<MockStage> root(new MockStage(ws.get()));

            for ( std::list<std::string>::const_iterator i = names.begin(); i != names.end(); ++i ) {
                string ns = *i;

                StringData collection = nsToCollectionSubstring( ns );
                if ( collection == "system.namespaces" ) {
                    continue;
                }

                BSONObjBuilder b;
                b.append( "name", collection );

                CollectionOptions options =
                    dbEntry->getCollectionCatalogEntry( txn, ns )->getCollectionOptions(txn);
                b.append( "options", options.toBSON() );

                BSONObj maybe = b.obj();
                if ( matcher && !matcher->matchesBSON( maybe ) ) {
                    continue;
                }

                WorkingSetID wsId = ws->allocate();
                WorkingSetMember* member = ws->get(wsId);
                member->state = WorkingSetMember::OWNED_OBJ;
                member->keyData.clear();
                member->loc = RecordId();
                member->obj = maybe;
                root->pushBack(*member);
            }

            std::string cursorNamespace = str::stream() << dbname << ".$cmd." << name;

            PlanExecutor* rawExec;
            Status makeStatus = PlanExecutor::make(txn,
                                                   ws.release(),
                                                   root.release(),
                                                   cursorNamespace,
                                                   PlanExecutor::YIELD_MANUAL,
                                                   &rawExec);
            std::auto_ptr<PlanExecutor> exec(rawExec);
            if (!makeStatus.isOK()) {
                return appendCommandStatus( result, makeStatus );
            }

            BSONElement batchSizeElem = jsobj.getFieldDotted("cursor.batchSize");
            const long long batchSize = batchSizeElem.isNumber()
                                        ? batchSizeElem.numberLong()
                                        : -1;

            BSONArrayBuilder firstBatch;

            const int byteLimit = MaxBytesToReturnToClientAtOnce;
            for (int objCount = 0;
                 firstBatch.len() < byteLimit && (batchSize == -1 || objCount < batchSize);
                 objCount++) {
                BSONObj next;
                PlanExecutor::ExecState state = exec->getNext(&next, NULL);
                if ( state == PlanExecutor::IS_EOF ) {
                    break;
                }
                invariant( state == PlanExecutor::ADVANCED );
                firstBatch.append(next);
            }

            CursorId cursorId = 0LL;
            if ( !exec->isEOF() ) {
                exec->saveState();
                ClientCursor* cursor = new ClientCursor(CursorManager::getGlobalCursorManager(),
                                                        exec.release());
                cursorId = cursor->cursorid();

                cursor->setOwnedRecoveryUnit(txn->releaseRecoveryUnit());
                StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
                txn->setRecoveryUnit(storageEngine->newRecoveryUnit());
            }

            Command::appendCursorResponseObject( cursorId, cursorNamespace, firstBatch.arr(),
                                                 &result );

            return true;
        }

    } cmdListCollections;

}
