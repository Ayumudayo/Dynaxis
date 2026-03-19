#include "server/storage/postgres/connection_pool.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "server/storage/connection_pool.hpp"
#include "server/storage/unit_of_work.hpp"
#include "server/storage/repositories.hpp"
#include <pqxx/pqxx>

/**
 * @brief Postgres 연결 풀/UnitOfWork/리포지터리 구현입니다.
 *
 * 도메인 저장소를 SQL 질의로 명시적으로 구현해,
 * 쿼리 동작과 트랜잭션 경계를 운영 관점에서 예측 가능하게 유지합니다.
 */
namespace server::storage::postgres {

using server::core::storage_execution::IUnitOfWork;
using server::core::storage_execution::PoolOptions;
using server::storage::IRepositoryConnectionPool;
using server::storage::IRepositoryUnitOfWork;
using server::storage::IUserRepository;
using server::storage::IRoomRepository;
using server::storage::IMessageRepository;
using server::storage::ISessionRepository;
using server::storage::IMembershipRepository;
using server::storage::User;
using server::storage::Room;
using server::storage::Message;
using server::storage::Session;

// users 테이블을 다루는 PostgreSQL 전용 Repository.
// SQL 쿼리를 직접 작성하여 DB와 상호작용합니다.
// ORM을 사용하지 않고 직접 SQL을 사용하는 이유는 명시적인 제어와 성능 최적화를 위해서입니다.
class PgUserRepository final : public IUserRepository {
public:
    explicit PgUserRepository(pqxx::work* w) : w_(w) {}

    std::optional<User> find_by_id(const std::string& user_id) override {
        auto r = w_->exec_params(
            "select id::text, name, (extract(epoch from created_at)*1000)::bigint from users where id = $1::uuid",
            user_id);
        if (r.empty()) return std::nullopt;
        User u{}; u.id = r[0][0].c_str(); u.name = r[0][1].c_str(); u.created_at_ms = r[0][2].as<std::int64_t>();
        return u;
    }

    std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) override {
        std::vector<User> out; out.reserve(limit);
        auto r = w_->exec_params(
            "select id::text, name, (extract(epoch from created_at)*1000)::bigint from users where lower(name)=lower($1) limit $2",
            name, static_cast<int>(limit));
        for (const auto& row : r) {
            User u{}; u.id = row[0].c_str(); u.name = row[1].c_str(); u.created_at_ms = row[2].as<std::int64_t>();
            out.emplace_back(std::move(u));
        }
        return out;
    }

    User create_guest(const std::string& name) override {
        auto r = w_->exec_params(
            "insert into users(id, name, password_hash, created_at) values (gen_random_uuid(), $1, '', now()) returning id::text, (extract(epoch from created_at)*1000)::bigint",
            name);
        User u{}; u.id = r[0][0].c_str(); u.name = name; u.created_at_ms = r[0][1].as<std::int64_t>();
        return u;
    }

    void update_last_login(const std::string& user_id,
                           const std::string& ip) override {
        w_->exec_params(
            "update users set last_login_ip = $2::inet, last_login_at = now() where id = $1::uuid",
            user_id, ip);
    }

private:
    pqxx::work* w_{};
};

// rooms 관련 CRUD/검색을 담당한다.
class PgRoomRepository final : public IRoomRepository {
public:
    explicit PgRoomRepository(pqxx::work* w) : w_(w) {}

    std::optional<Room> find_by_id(const std::string& room_id) override {
        auto r = w_->exec_params(
            "select id::text, name, is_public, is_active, (extract(epoch from closed_at)*1000)::bigint, (extract(epoch from created_at)*1000)::bigint from rooms where id=$1::uuid",
            room_id);
        if (r.empty()) return std::nullopt;
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = r[0][1].c_str(); rm.is_public = r[0][2].as<bool>(); rm.is_active = r[0][3].as<bool>();
        if (!r[0][4].is_null()) rm.closed_at_ms = r[0][4].as<std::int64_t>(); rm.created_at_ms = r[0][5].as<std::int64_t>();
        return rm;
    }

    std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) override {
        std::vector<Room> out; out.reserve(limit);
        auto r = w_->exec_params(
            "select id::text, name, is_public, is_active, (extract(epoch from created_at)*1000)::bigint from rooms where lower(name) like lower($1) order by created_at desc limit $2",
            "%" + query + "%", static_cast<int>(limit));
        for (const auto& row : r) {
            Room rm{}; rm.id = row[0].c_str(); rm.name = row[1].c_str(); rm.is_public = row[2].as<bool>(); rm.is_active = row[3].as<bool>(); rm.created_at_ms = row[4].as<std::int64_t>();
            out.emplace_back(std::move(rm));
        }
        return out;
    }

    std::optional<Room> find_by_name_exact_ci(const std::string& name) override {
        auto r = w_->exec_params(
            "select id::text, name, is_public, is_active, (extract(epoch from created_at)*1000)::bigint from rooms where lower(name)=lower($1) and is_active=true order by created_at asc limit 1",
            name);
        if (r.empty()) return std::nullopt;
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = r[0][1].c_str(); rm.is_public = r[0][2].as<bool>(); rm.is_active = r[0][3].as<bool>(); rm.created_at_ms = r[0][4].as<std::int64_t>();
        return rm;
    }

    Room create(const std::string& name, bool is_public) override {
        auto r = w_->exec_params(
            "insert into rooms(id, name, is_public, is_active, created_at) values (gen_random_uuid(), $1, $2, true, now()) returning id::text, (extract(epoch from created_at)*1000)::bigint",
            name, is_public);
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = name; rm.is_public = is_public; rm.is_active = true; rm.created_at_ms = r[0][1].as<std::int64_t>();
        return rm;
    }

    void close(const std::string& room_id) override {
        w_->exec_params(
            "update rooms set is_active=false, closed_at=now() where id=$1::uuid",
            room_id);
    }

private:
    pqxx::work* w_{};
};

// messages 테이블 접근 레이어.
class PgMessageRepository final : public IMessageRepository {
public:
    explicit PgMessageRepository(pqxx::work* w) : w_(w) {}

    std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                              std::uint64_t since_id,
                                              std::size_t limit) override {
        std::vector<Message> out; out.reserve(limit);
        auto r = w_->exec_params(
            "select m.id, m.room_id::text, coalesce(m.room_name, ''), coalesce(m.user_id::text, ''), m.content, (extract(epoch from m.created_at)*1000)::bigint, coalesce(u.name,'') "
            "from messages m left join users u on u.id = m.user_id "
            "where m.room_id=$1::uuid and m.id > $2 order by m.id asc limit $3",
            room_id, static_cast<long long>(since_id), static_cast<int>(limit));
        for (const auto& row : r) {
            Message m{}; m.id = row[0].as<std::uint64_t>(); m.room_id = row[1].c_str(); m.room_name = row[2].c_str();
            auto uid = row[3].c_str(); if (uid && *uid) m.user_id = std::string(uid);
            m.content = row[4].c_str(); m.created_at_ms = row[5].as<std::int64_t>();
            auto uname = row[6].c_str(); if (uname && *uname) m.user_name = std::string(uname);
            out.emplace_back(std::move(m));
        }
        return out;
    }

    Message create(const std::string& room_id,
                   const std::string& room_name,
                   const std::optional<std::string>& user_id,
                   const std::string& content) override {
        pqxx::result r;
        if (user_id) {
            r = w_->exec_params(
                "insert into messages(room_id, room_name, user_id, content) values ($1::uuid, $2, $3::uuid, $4) returning id, (extract(epoch from created_at)*1000)::bigint",
                room_id, room_name, *user_id, content);
        } else {
            r = w_->exec_params(
                "insert into messages(room_id, room_name, user_id, content) values ($1::uuid, $2, NULL, $3) returning id, (extract(epoch from created_at)*1000)::bigint",
                room_id, room_name, content);
        }
        Message m{}; m.id = r[0][0].as<std::uint64_t>(); m.room_id = room_id; m.room_name = room_name; if (user_id) m.user_id = *user_id; m.content = content; m.created_at_ms = r[0][1].as<std::int64_t>();
        return m;
    }

    std::uint64_t get_last_id(const std::string& room_id) override {
        auto r = w_->exec_params(
            "select coalesce(max(id), 0) from messages where room_id=$1::uuid",
            room_id);
        return r[0][0].as<std::uint64_t>();
    }

    void delete_by_room(const std::string& room_id) override {
        w_->exec_params("delete from messages where room_id=$1::uuid", room_id);
    }

private:
    pqxx::work* w_{};
};

// sessions 테이블을 다루는 Repository.
class PgSessionRepository final : public ISessionRepository {
public:
    explicit PgSessionRepository(pqxx::work* w) : w_(w) {}

    std::optional<Session> find_by_token_hash(const std::string& token_hash) override {
        auto r = w_->exec_params(
            "select id::text, user_id::text, encode(token_hash,'hex'), (extract(epoch from created_at)*1000)::bigint, (extract(epoch from expires_at)*1000)::bigint, (extract(epoch from revoked_at)*1000)::bigint from sessions where token_hash = decode($1,'hex') limit 1",
            token_hash);
        if (r.empty()) return std::nullopt;
        Session s{}; s.id = r[0][0].c_str(); s.user_id = r[0][1].c_str(); s.token_hash = r[0][2].c_str(); s.created_at_ms = r[0][3].as<std::int64_t>(); s.expires_at_ms = r[0][4].as<std::int64_t>(); if (!r[0][5].is_null()) s.revoked_at_ms = r[0][5].as<std::int64_t>();
        return s;
    }

    Session create(const std::string& user_id,
                   const std::chrono::system_clock::time_point& expires_at,
                   const std::optional<std::string>& client_ip,
                   const std::optional<std::string>& user_agent,
                   const std::string& token_hash) override {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
        auto r = w_->exec_params(
            "insert into sessions(id, user_id, token_hash, client_ip, user_agent, created_at, expires_at) values (gen_random_uuid(), $1::uuid, decode($2,'hex'), $3::inet, $4, now(), to_timestamp($5/1000.0)) returning id::text, (extract(epoch from created_at)*1000)::bigint",
            user_id, token_hash, client_ip.value_or(""), user_agent.value_or(""), static_cast<long long>(ms));
        Session s{}; s.id = r[0][0].c_str(); s.user_id = user_id; s.token_hash = token_hash; s.created_at_ms = r[0][1].as<std::int64_t>(); s.expires_at_ms = ms; return s;
    }

    void revoke(const std::string& session_id) override {
        w_->exec_params("update sessions set revoked_at = now() where id = $1::uuid", session_id);
    }

private:
    pqxx::work* w_{};
};

// memberships 테이블에서 last_seen 등 멤버 상태를 관리한다.
class PgMembershipRepository final : public IMembershipRepository {
public:
    explicit PgMembershipRepository(pqxx::work* w) : w_(w) {}

    void upsert_join(const std::string& user_id,
                     const std::string& room_id,
                     const std::string& role) override {
        w_->exec_params(
            "insert into memberships(user_id, room_id, role, joined_at, is_member) "
            "values ($1::uuid, $2::uuid, $3, now(), true) "
            "on conflict (user_id, room_id) do update set role=excluded.role, joined_at=now(), is_member=true, left_at=null",
            user_id, room_id, role);
    }

    void update_last_seen(const std::string& user_id,
                          const std::string& room_id,
                          std::uint64_t last_seen_msg_id) override {
        w_->exec_params(
            "update memberships set last_seen_msg_id = $3 where user_id=$1::uuid and room_id=$2::uuid",
            user_id, room_id, static_cast<long long>(last_seen_msg_id));
    }

    void leave(const std::string& user_id,
               const std::string& room_id) override {
        w_->exec_params(
            "update memberships set is_member=false, left_at=now() where user_id=$1::uuid and room_id=$2::uuid",
            user_id, room_id);
    }

    std::optional<std::uint64_t> get_last_seen(const std::string& user_id,
                                               const std::string& room_id) override {
        auto r = w_->exec_params(
            "select last_seen_msg_id from memberships where user_id=$1::uuid and room_id=$2::uuid",
            user_id, room_id);
        if (r.empty() || r[0][0].is_null()) return std::nullopt;
        return r[0][0].as<std::uint64_t>();
    }

private:
    pqxx::work* w_{};
};

// pqxx::work 하나에 모든 Repository를 묶어 transaction을 관리한다.
// UnitOfWork 패턴을 구현하여, 여러 리포지토리의 변경사항이 하나의 트랜잭션으로 묶이도록 보장합니다.
// commit()을 호출하기 전까지는 DB에 반영되지 않으며, 예외 발생 시 자동 롤백됩니다.
class PgRepositoryUnitOfWork final : public IRepositoryUnitOfWork {
public:
    explicit PgRepositoryUnitOfWork(std::shared_ptr<pqxx::connection> conn)
        : conn_(std::move(conn)), w_(*conn_),
          users_(&w_), rooms_(&w_), messages_(&w_), sessions_(&w_), memberships_(&w_) {}

    void commit() override { w_.commit(); }
    void rollback() override { w_.abort(); }

    IUserRepository& users() override { return users_; }
    IRoomRepository& rooms() override { return rooms_; }
    IMessageRepository& messages() override { return messages_; }
    ISessionRepository& sessions() override { return sessions_; }
    IMembershipRepository& memberships() override { return memberships_; }

private:
    std::shared_ptr<pqxx::connection> conn_{};
    pqxx::work w_;
    PgUserRepository users_;
    PgRoomRepository rooms_;
    PgMessageRepository messages_;
    PgSessionRepository sessions_;
    PgMembershipRepository memberships_;
};

// 간단한 연결 팩토리 구현: 요청마다 pqxx connection을 생성한다.
class PgConnectionPool final : public IRepositoryConnectionPool {
public:
    PgConnectionPool(std::string db_uri, PoolOptions opts)
        : db_uri_(std::move(db_uri)), opts_(opts) {}

    std::unique_ptr<IRepositoryUnitOfWork> make_repository_unit_of_work() override {
        // 요청마다 새로운 pqxx::connection을 열어 트랜잭션 경계를 분리한다.
        // 실제 운영 환경에서는 커넥션 풀링 라이브러리(예: pgBouncer)를 앞단에 두거나,
        // 내부적으로 커넥션 객체를 재사용하는 풀을 구현해야 성능 오버헤드를 줄일 수 있습니다.
        // 현재 구현은 단순함을 위해 매번 연결을 생성합니다.
        auto conn = std::make_shared<pqxx::connection>(db_uri_);
        if (!conn->is_open()) throw std::runtime_error("PQXX connection failed");
        return std::make_unique<PgRepositoryUnitOfWork>(std::move(conn));
    }

    bool health_check() override {
        // 간단한 SELECT 1로 연결 가능 여부를 확인한다.
        try {
            pqxx::connection c(db_uri_);
            if (!c.is_open()) return false;
            pqxx::work w(c);
            auto r = w.exec("select 1");
            (void)r; w.commit();
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::string db_uri_;
    PoolOptions opts_{};
};

std::shared_ptr<IRepositoryConnectionPool>
make_connection_pool_impl(const std::string& db_uri, const PoolOptions& opts) {
    return std::make_shared<PgConnectionPool>(db_uri, opts);
}

} // namespace server::storage::postgres
