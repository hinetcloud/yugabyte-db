//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//--------------------------------------------------------------------------------------------------

#ifndef YB_SQL_TEST_YBSQL_TEST_BASE_H_
#define YB_SQL_TEST_YBSQL_TEST_BASE_H_

#include "yb/sql/sql_processor.h"
#include "yb/sql/util/sql_env.h"

#include "yb/client/client.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/mini_master.h"

#include "yb/util/test_util.h"

namespace yb {
namespace sql {

#define ANALYZE_VALID_STMT(sql_env, sql_stmt, parse_tree)             \
do {                                                                  \
  Status s = TestAnalyzer(sql_env, sql_stmt, parse_tree);             \
  EXPECT_TRUE(s.ok());                                                \
} while (false)

#define ANALYZE_INVALID_STMT(sql_env, sql_stmt, parse_tree)           \
do {                                                                  \
  Status s = TestAnalyzer(sql_env, sql_stmt, parse_tree);             \
  EXPECT_FALSE(s.ok());                                               \
} while (false)

#define PARSE_VALID_STMT(sql_stmt)             \
do {                                           \
  Status s = TestParser(sql_stmt);             \
  EXPECT_TRUE(s.ok());                         \
} while (false)

#define PARSE_INVALID_STMT(sql_stmt)           \
do {                                           \
  Status s = TestParser(sql_stmt);             \
  EXPECT_FALSE(s.ok());                        \
} while (false)

#define EXEC_VALID_STMT(sql_stmt)              \
do {                                           \
  Status s = processor->Run(sql_stmt);         \
  EXPECT_TRUE(s.ok());                         \
} while (false)

#define EXEC_INVALID_STMT(sql_stmt)            \
do {                                           \
  Status s = processor->Run(sql_stmt);         \
  EXPECT_FALSE(s.ok());                        \
} while (false)

#define CHECK_VALID_STMT(sql_stmt)              \
do {                                            \
  Status s = processor->Run(sql_stmt);          \
  CHECK(s.ok()) << "Failure: " << s.ToString(); \
} while (false)

#define CHECK_INVALID_STMT(sql_stmt)            \
do {                                            \
  Status s = processor->Run(sql_stmt);          \
  CHECK(!s.ok()) << "Expect failure";           \
} while (false)

class YbSqlProcessor : public SqlProcessor {
 public:
  // Public types.
  typedef std::unique_ptr<YbSqlProcessor> UniPtr;
  typedef std::unique_ptr<const YbSqlProcessor> UniPtrConst;

  // Constructors.
  explicit YbSqlProcessor(
      std::shared_ptr<client::YBClient> client, std::shared_ptr<client::YBTableCache> cache)
      : SqlProcessor(client, cache, nullptr /* sql_metrics */) { }
  virtual ~YbSqlProcessor() { }

  // Execute a SQL statement.
  CHECKED_STATUS Run(const std::string& sql_stmt) {
    result_ = nullptr;
    return SqlProcessor::Run(sql_stmt, StatementParameters(), &result_);
  }

    // Construct a row_block and send it back.
  std::shared_ptr<YQLRowBlock> row_block() const {
    if (result_ != nullptr && result_->type() == ExecuteResult::Type::ROWS) {
      return std::shared_ptr<YQLRowBlock>(static_cast<RowsResult*>(result_.get())->GetRowBlock());
    }
    return nullptr;
  }

 private:
  // Execute result.
  ExecuteResult::UniPtr result_;
};

// Base class for all SQL test cases.
class YbSqlTestBase : public YBTest {
 public:
  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  YbSqlTestBase();
  ~YbSqlTestBase();

  //------------------------------------------------------------------------------------------------
  // Test start and cleanup functions.
  virtual void SetUp() OVERRIDE {
    YBTest::SetUp();
  }
  virtual void TearDown() OVERRIDE {
    if (cluster_ != nullptr) {
      cluster_->Shutdown();
    }
    YBTest::TearDown();
  }

  //------------------------------------------------------------------------------------------------
  // Test only the parser.
  CHECKED_STATUS TestParser(const std::string& sql_stmt) {
    SqlProcessor *processor = GetSqlProcessor();
    ParseTree::UniPtr parse_tree;
    return processor->Parse(sql_stmt, &parse_tree, nullptr /* mem_tracker */);
  }

  // Tests parser and analyzer
  CHECKED_STATUS TestAnalyzer(SqlEnv *sql_env, const string& sql_stmt,
                              ParseTree::UniPtr *parse_tree) {
    SqlProcessor *processor = GetSqlProcessor();
    RETURN_NOT_OK(processor->Parse(sql_stmt, parse_tree, nullptr /* mem_tracker */));
    RETURN_NOT_OK(processor->Analyze(sql_stmt, parse_tree, false /* refresh_cache */));
    return Status::OK();
  }

  //------------------------------------------------------------------------------------------------
  // Create simulated cluster.
  void CreateSimulatedCluster();

  // Create sql processor.
  YbSqlProcessor *GetSqlProcessor();

  // Create a session context for client_.
  SqlEnv *CreateSqlEnv();

  // Pull a session from the cached tables.
  SqlEnv *GetSqlEnv(int session_id);

 protected:
  //------------------------------------------------------------------------------------------------

  // Simulated cluster.
  std::shared_ptr<MiniCluster> cluster_;

  // Simulated YB client.
  std::shared_ptr<client::YBClient> client_;
  std::shared_ptr<client::YBTableCache> table_cache_;

  SqlSession::SharedPtr sql_session_;

  // Contexts to be passed to SQL engine.
  std::vector<SqlEnv::UniPtr> sql_envs_;

  // SQL Processor.
  std::vector<YbSqlProcessor::UniPtr> sql_processors_;
};

}  // namespace sql
}  // namespace yb

#endif  // YB_SQL_TEST_YBSQL_TEST_BASE_H_
