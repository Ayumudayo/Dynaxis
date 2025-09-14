#include "server/storage/postgres/connection_pool.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/unit_of_work.hpp"
#include "server/core/storage/repositories.hpp"

#if defined(HAVE_LIBPQXX)
#include <pqxx/pqxx>
#endif

namespace server::storage::postgres {

using server::core::storage::IConnectionPool;
using server::core::storage::IUnitOfWork;
using server::core::storage::PoolOptions;
using server::core::storage::IUserRepository;
using server::core::storage::IRoomRepository;
using server::core::storage::IMessageRepository;
using server::core::storage::ISessionRepository;
using server::core::storage::IMembershipRepository;
using server::core::storage::User;
using server::core::storage::Room;
using server::core::storage::Message;
using server::core::storage::Session;

class PgUserRepository final : public IUserRepository {
public:
#if defined(HAVE_LIBPQXX)
    explicit PgUserRepository(pqxx::work* w) : w_(w) {}
#else
    PgUserRepository() = default;
#endif

    std::optional<User> find_by_id(const std::string& user_id) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("select id::text, name, (extract(epoch from created_at)*1000)::bigint from users where id = $1::uuid", user_id);
        if (r.empty()) return std::nullopt;
        User u{}; u.id = r[0][0].c_str(); u.name = r[0][1].c_str(); u.created_at_ms = r[0][2].as<std::int64_t>();
        return u;
#else
        (void)user_id; return std::nullopt;
#endif
    }

    std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) override {
#if defined(HAVE_LIBPQXX)
        std::vector<User> out; out.reserve(limit);
        auto r = w_->exec_params("select id::text, name, (extract(epoch from created_at)*1000)::bigint from users where lower(name)=lower($1) limit $2", name, static_cast<int>(limit));
        for (const auto& row : r) { User u{}; u.id = row[0].c_str(); u.name = row[1].c_str(); u.created_at_ms = row[2].as<std::int64_t>(); out.emplace_back(std::move(u)); }
        return out;
#else
        (void)name; (void)limit; return {};
#endif
    }

    User create_guest(const std::string& name) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("insert into users(id, name, password_hash, created_at) values (gen_random_uuid(), $1, '', now()) returning id::text, (extract(epoch from created_at)*1000)::bigint", name);
        User u{}; u.id = r[0][0].c_str(); u.name = name; u.created_at_ms = r[0][1].as<std::int64_t>();
        return u;
#else
        (void)name; return {};
#endif
    }

#if defined(HAVE_LIBPQXX)
private:
    pqxx::work* w_{};
#endif
};

class PgRoomRepository final : public IRoomRepository {
public:
    PgRoomRepository(
#if defined(HAVE_LIBPQXX)
        pqxx::work* w
#endif
    )
#if defined(HAVE_LIBPQXX)
        : w_(w)
#endif
    {}

    std::optional<Room> find_by_id(const std::string& room_id) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("select id::text, name, is_public, is_active, (extract(epoch from closed_at)*1000)::bigint, (extract(epoch from created_at)*1000)::bigint from rooms where id=$1::uuid", room_id);
        if (r.empty()) return std::nullopt;
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = r[0][1].c_str(); rm.is_public = r[0][2].as<bool>(); rm.is_active = r[0][3].as<bool>(); if (!r[0][4].is_null()) rm.closed_at_ms = r[0][4].as<std::int64_t>(); rm.created_at_ms = r[0][5].as<std::int64_t>();
        return rm;
#else
        (void)room_id; return std::nullopt;
#endif
    }
    std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) override {
#if defined(HAVE_LIBPQXX)
        std::vector<Room> out; out.reserve(limit);
        auto r = w_->exec_params("select id::text, name, is_public, is_active, (extract(epoch from created_at)*1000)::bigint from rooms where lower(name) like lower($1) order by created_at desc limit $2", "%" + query + "%", static_cast<int>(limit));
        for (const auto& row : r) { Room rm{}; rm.id = row[0].c_str(); rm.name = row[1].c_str(); rm.is_public = row[2].as<bool>(); rm.is_active = row[3].as<bool>(); rm.created_at_ms = row[4].as<std::int64_t>(); out.emplace_back(std::move(rm)); }
        return out;
#else
        (void)query; (void)limit; return {};
#endif
    }
    std::optional<Room> find_by_name_exact_ci(const std::string& name) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("select id::text, name, is_public, is_active, (extract(epoch from created_at)*1000)::bigint from rooms where lower(name)=lower($1) order by created_at asc limit 1", name);
        if (r.empty()) return std::nullopt;
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = r[0][1].c_str(); rm.is_public = r[0][2].as<bool>(); rm.is_active = r[0][3].as<bool>(); rm.created_at_ms = r[0][4].as<std::int64_t>();
        return rm;
#else
        (void)name; return std::nullopt;
#endif
    }
    Room create(const std::string& name, bool is_public) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("insert into rooms(id, name, is_public, is_active, created_at) values (gen_random_uuid(), $1, $2, true, now()) returning id::text, (extract(epoch from created_at)*1000)::bigint", name, is_public);
        Room rm{}; rm.id = r[0][0].c_str(); rm.name = name; rm.is_public = is_public; rm.is_active = true; rm.created_at_ms = r[0][1].as<std::int64_t>();
        return rm;
#else
        (void)name; (void)is_public; return {};
#endif
    }
#if defined(HAVE_LIBPQXX)
private:
    pqxx::work* w_{};
#endif
};

class PgMessageRepository final : public IMessageRepository {
public:
    PgMessageRepository(
#if defined(HAVE_LIBPQXX)
        pqxx::work* w
#endif
    )
#if defined(HAVE_LIBPQXX)
        : w_(w)
#endif
    {}

    std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                              std::uint64_t since_id,
                                              std::size_t limit) override {
#if defined(HAVE_LIBPQXX)
        std::vector<Message> out; out.reserve(limit);
        auto r = w_->exec_params(
            "select id, room_id::text, coalesce(room_name, ''), coalesce(user_id::text, ''), content, (extract(epoch from created_at)*1000)::bigint "
            "from messages where room_id=$1::uuid and id > $2 order by id asc limit $3",
            room_id, static_cast<long long>(since_id), static_cast<int>(limit));
        for (const auto& row : r) {
            Message m{}; m.id = row[0].as<std::uint64_t>(); m.room_id = row[1].c_str(); m.room_name = row[2].c_str();
            auto uid = row[3].c_str(); if (uid && *uid) m.user_id = std::string(uid);
            m.content = row[4].c_str(); m.created_at_ms = row[5].as<std::int64_t>();
            out.emplace_back(std::move(m));
        }
        return out;
#else
        (void)room_id; (void)since_id; (void)limit; return {};
#endif
    }
    Message create(const std::string& room_id,
                   const std::string& room_name,
                   const std::optional<std::string>& user_id,
                   const std::string& content) override {
#if defined(HAVE_LIBPQXX)
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
#else
        (void)room_id; (void)room_name; (void)user_id; (void)content; return {};
#endif
    }
#if defined(HAVE_LIBPQXX)
private:
    pqxx::work* w_{};
#endif
};

class PgSessionRepository final : public ISessionRepository {
public:
    PgSessionRepository(
#if defined(HAVE_LIBPQXX)
        pqxx::work* w
#endif
    )
#if defined(HAVE_LIBPQXX)
        : w_(w)
#endif
    {}

    std::optional<Session> find_by_token_hash(const std::string& token_hash) override {
#if defined(HAVE_LIBPQXX)
        auto r = w_->exec_params("select id::text, user_id::text, encode(token_hash,'hex'), (extract(epoch from created_at)*1000)::bigint, (extract(epoch from expires_at)*1000)::bigint, (extract(epoch from revoked_at)*1000)::bigint from sessions where token_hash = decode($1,'hex') limit 1", token_hash);
        if (r.empty()) return std::nullopt;
        Session s{}; s.id = r[0][0].c_str(); s.user_id = r[0][1].c_str(); s.token_hash = r[0][2].c_str(); s.created_at_ms = r[0][3].as<std::int64_t>(); s.expires_at_ms = r[0][4].as<std::int64_t>(); if (!r[0][5].is_null()) s.revoked_at_ms = r[0][5].as<std::int64_t>();
        return s;
#else
        (void)token_hash; return std::nullopt;
#endif
    }
    Session create(const std::string& user_id,
                   const std::chrono::system_clock::time_point& expires_at,
                   const std::optional<std::string>& client_ip,
                   const std::optional<std::string>& user_agent,
                   const std::string& token_hash) override {
#if defined(HAVE_LIBPQXX)
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
        auto r = w_->exec_params(
            "insert into sessions(id, user_id, token_hash, client_ip, user_agent, created_at, expires_at) values (gen_random_uuid(), $1::uuid, decode($2,'hex'), $3::inet, $4, now(), to_timestamp($5/1000.0)) returning id::text, (extract(epoch from created_at)*1000)::bigint",
            user_id, token_hash, client_ip.value_or(""), user_agent.value_or(""), static_cast<long long>(ms));
        Session s{}; s.id = r[0][0].c_str(); s.user_id = user_id; s.token_hash = token_hash; s.created_at_ms = r[0][1].as<std::int64_t>(); s.expires_at_ms = ms; return s;
#else
        (void)user_id; (void)expires_at; (void)client_ip; (void)user_agent; (void)token_hash; return {};
#endif
    }
    void revoke(const std::string& session_id) override {
#if defined(HAVE_LIBPQXX)
        w_->exec_params("update sessions set revoked_at = now() where id = $1::uuid", session_id);
#else
        (void)session_id;
#endif
    }
#if defined(HAVE_LIBPQXX)
private:
    pqxx::work* w_{};
#endif
};
class PgMembershipRepository final : public IMembershipRepository {
public:
#if defined(HAVE_LIBPQXX)
    explicit PgMembershipRepository(pqxx::work* w) : w_(w) {}
#else
    PgMembershipRepository() = default;
#endif

    void upsert_join(const std::string& user_id,
                     const std::string& room_id,
                     const std::string& role) override {
#if defined(HAVE_LIBPQXX)
        w_->exec_params(
            "insert into memberships(user_id, room_id, role, joined_at, is_member) "
            "values (::uuid, ::uuid, , now(), true) "
            "on conflict (user_id, room_id) do update set role=excluded.role, joined_at=now(), is_member=true, left_at=null",
            user_id, room_id, role);
#else
        (void)user_id; (void)room_id; (void)role;
#endif
    }

    void update_last_seen(const std::string& user_id,
                          const std::string& room_id,
                          std::uint64_t last_seen_msg_id) override {
        w_->exec_params(
            "update memberships set last_seen_msg_id =  where user_id=::uuid and room_id=::uuid",
            user_id, room_id, static_cast<long long>(last_seen_msg_id));
            user_id, room_id, static_cast<long long>(last_seen_msg_id));
#else
        (void)user_id; (void)room_id; (void)last_seen_msg_id;
#endif
    }

    void leave(const std::string& user_id,
               const std::string& room_id) override {
        w_->exec_params(
            "update memberships set is_member=false, left_at=now() where user_id=::uuid and room_id=::uuid",
            user_id, room_id);
            user_id, room_id);
#else
        (void)user_id; (void)room_id;
#endif
    }

#if defined(HAVE_LIBPQXX)
private:
    pqxx::work* w_{};
#endif
};

class PgMembershipRepository; // forward
class PgUnitOfWork final : public IUnitOfWork {
public:
    PgUnitOfWork(
#if defined(HAVE_LIBPQXX)
        std::shared_ptr<pqxx::connection> conn
#endif
    )
#if defined(HAVE_LIBPQXX)
        : conn_(std::move(conn)), w_(*conn_)
#endif
    {}

    void commit() override {
        #if defined(HAVE_LIBPQXX)
        w_.commit();
        #endif
    }
    void rollback() override {
        #if defined(HAVE_LIBPQXX)
        w_.abort();
        #endif
    }

    IUserRepository& users() override { return users_; }
    IRoomRepository& rooms() override { return rooms_; }
    IMessageRepository& messages() override { return messages_; }
    IMembershipRepository& memberships() override { return memberships_; }
    ISessionRepository& sessions() override { return sessions_; }

private:
    #if defined(HAVE_LIBPQXX)
    std::shared_ptr<pqxx::connection> conn_{};
    pqxx::work w_;
    #endif
    PgUserRepository users_
    #if defined(HAVE_LIBPQXX)
    { &w_ }
    #endif
    ;
    PgRoomRepository rooms_
    #if defined(HAVE_LIBPQXX)
    { &w_ }
    #endif
    ;
    PgMessageRepository messages_
    #if defined(HAVE_LIBPQXX)
    { &w_ }
    #endif
    ;
    PgSessionRepository sessions_
    #if defined(HAVE_LIBPQXX)
    { &w_ }
    PgMembershipRepository memberships_
    #if defined(HAVE_LIBPQXX)
    { &w_ }
    #endif
    ;
    #endif
    ;
};

class PgConnectionPool final : public IConnectionPool {
public:
    PgConnectionPool(std::string db_uri, PoolOptions opts)
        : db_uri_(std::move(db_uri)), opts_(opts) {}

    std::unique_ptr<IUnitOfWork> make_unit_of_work() override {
        #if defined(HAVE_LIBPQXX)
        auto conn = std::make_shared<pqxx::connection>(db_uri_);
        if (!conn->is_open()) throw std::runtime_error("PQXX connection failed");
        return std::make_unique<PgUnitOfWork>(std::move(conn));
        #else
        return std::make_unique<PgUnitOfWork>();
        #endif
    }

    bool health_check() override {
        #if defined(HAVE_LIBPQXX)
        try {
            pqxx::connection c(db_uri_);
            if (!c.is_open()) return false;
            pqxx::work w(c);
            auto r = w.exec("select 1");
            (void)r; w.commit();
            return true;
        } catch (...) { return false; }
        #else
        return true;
        #endif
    }

private:
    std::string db_uri_;
    PoolOptions opts_{};
};

std::shared_ptr<IConnectionPool>
make_connection_pool(const std::string& db_uri, const PoolOptions& opts) {
    return std::make_shared<PgConnectionPool>(db_uri, opts);
}

} // namespace server::storage::postgres

