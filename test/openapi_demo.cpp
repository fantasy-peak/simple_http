// OpenAPI demo: a small but realistic petshop API to exercise the whole
// feature — typed routes (`route<Req, Res>`) collected into an OAS 3.1
// document, path-template routing (`/pets/{id}`) whose captures become path
// parameters automatically, query/header parameters, error responses, and a
// security scheme behind the write endpoints. Served at /openapi.json and
// browsed through CDN-hosted Swagger UI at /swagger:
//
//   xmake build openapi_demo && xmake run openapi_demo
//   open http://127.0.0.1:7795/swagger

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <glaze/glaze.hpp>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include "simple_http.h"

namespace asio = boost::asio;
using namespace simple_http;

// --- domain types: glass can reflect these with zero metadata, so each one's
// JSON Schema is automatic. ---------------------------------------------------

struct Pet {
    std::int64_t id{};
    std::string name;
    std::string status{"available"}; // available | pending | sold
    // Field-level metadata (gap 2): documented in the schema and rendered by
    // swagger-ui as a description + an enum dropdown for this field.
    struct glaze_json_schema {
        glz::schema status{.description = "lifecycle state of the pet",
                           .enumeration = std::vector<std::string_view>{"available", "pending", "sold"}};
    };
};

struct PetList {
    std::vector<Pet> items;
};

struct CreatePetReq {
    std::string name;
    std::optional<std::string> status{}; // optional: omitting it uses "available"
};

struct Order {
    std::int64_t id{};
    std::int64_t pet_id{};
    int quantity{1};
    std::string status{"placed"};
};

struct CreateOrderReq {
    std::int64_t pet_id{};
    std::optional<int> quantity{}; // optional: omitting it uses 1
};

// The body every error answer shares.
struct ErrorBody {
    std::string error;
};

// Path-parameter structs: each field's name must equal a `{name}` in its route
// template (checked at registration), exactly the axum `Path<OrderItemParams>`
// contract — the field type is also the document's path-parameter schema.
struct PetParams {
    std::int64_t id{};
};

struct OwnerPetParams {
    std::string owner; // a slug like "acme-corp" — strings are the common path part
    std::uint64_t pet_id{};
};

struct OrderItemParams {
    std::uint64_t order_id{};
    std::uint64_t item_id{};
};

// The trickier cases: mixed scalar path-parameter types, parsed by type.
struct GeoParams {
    std::string city;
    double lat{};
    double lng{};
    std::uint32_t precision{6};
};
struct GeoReply {
    std::string city;
    double lat{};
    double lng{};
    std::uint32_t precision{};
};
struct LabParams {
    std::string experiment;
    std::uint32_t run{};
    bool fast{};
};
struct LabReply {
    std::string experiment;
    std::uint32_t run{};
    bool fast{}; // parses "true"/"false"; anything else is a 404
};
struct OwnerParams {
    std::uint64_t owner_id{};
};

// --- helpers -----------------------------------------------------------------

template <typename T> std::string to_json(const T &value) {
    if (auto out = glz::write_json(value)) {
        return *out;
    }
    return "{}";
}

// Reads and validates the request body against the declared Req type and
// answers 400/422 with the API's ErrorBody shape on failure (returning nullopt
// so the handler returns). Validation itself is the library's read_body — the
// runtime half of the document's requestBody schema.
template <typename TReq> asio::awaitable<std::optional<TReq>> read_body_api(RequestPtr req, ResponsePtr res) {
    openapi::BodyError err;
    auto in = co_await openapi::read_body<TReq>(*req, err);
    if (!in) {
        co_await res->status(err.status).content_type(mime::app_json).send(to_json(ErrorBody{.error = err.message}));
    }
    co_return in;
}

// Demo "database": path params and bodies resolve against fixed records.
std::int64_t parse_id(RequestPtr req, std::string_view name, std::int64_t fallback) {
    const auto v = req->param(name);
    return v ? std::strtoll(std::string{*v}.c_str(), nullptr, 10) : fallback;
}

bool authorized(RequestPtr req) {
    const auto auth = req->header("authorization");
    return auth.has_value() && auth->starts_with("Bearer ");
}

namespace {

volatile std::sig_atomic_t g_stop = 0;

void request_stop(int) { g_stop = 1; }

} // namespace

// A bearer challenge the demo write endpoints answer when the header is wrong,
// so the security declaration in the document matches what the server does.
asio::awaitable<void> unauthorized(ResponsePtr res) {
    co_await res->status(401)
        .header("www-authenticate", "Bearer realm=\"petshop\"")
        .content_type(mime::app_json)
        .send(to_json(ErrorBody{.error = "missing or invalid bearer token"}));
    co_return;
}

int main() {
    ServerConfig cfg;
    cfg.listen = InetAddress{"127.0.0.1", 7795, false};
    cfg.worker_threads = 4;
    Server server{cfg};

    server.openapi()
        .title("petshop")
        .version("1.0.0")
        .server("http://127.0.0.1:7795")
        // The Authorize button in Swagger UI comes from this.
        .security_scheme("bearer", openapi::bearer());

    // --- GET /pets?status=&limit= — bodyless, query-parameterized ------------
    server.route<PetList>(
        {Method::Get}, "/pets",
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            PetList list;
            list.items.emplace_back(Pet{.id = 10, .name = "rex", .status = "available"});
            co_await res->status(200).content_type(mime::app_json).send(to_json(list));
        },
        openapi::OperationInfo{.summary = "list pets",
                               .tag = "pet",
                               .description = "All pets, optionally filtered by status.",
                               .operation_id = "listPets"},
        openapi::query<std::string>("status", {.description = "available | pending | sold"}),
        openapi::query<std::int32_t>("limit", {.required = false, .description = "page size"}));

    // --- GET /pets/{id} — the template becomes a path parameter ---------------
    server.route<PetParams, openapi::NoBody, Pet>(
        {Method::Get}, "/pets/{id}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<PetParams>(*req);
            if (!p || p->id <= 0) {
                co_await res->status(404).content_type(mime::app_json).send(to_json(ErrorBody{.error = "no such pet"}));
                co_return;
            }
            // The declared request header, read for real: a
            // correlation id that is also echoed back as a
            // response header, so the round trip is visible.
            const auto trace = req->header("x-trace-id");
            auto out = res->status(200).content_type(mime::app_json);
            if (trace) {
                out.header("x-trace-id", std::string{*trace});
            }
            co_await out.send(to_json(Pet{.id = p->id, .name = "rex", .status = "available"}));
        },
        // The `id` path parameter comes from PetParams: name from
        // the field, schema from its type, required. Nothing to
        // declare by hand.
        openapi::OperationInfo{.summary = "get a pet", .tag = "pet", .operation_id = "getPet"},
        openapi::header<std::string>("x-trace-id", {.description = "correlation id, echoed back"}),
        openapi::resp<ErrorBody>(404, "no such pet"));

    // --- GET /owners/{owner_id}/pets/{pet_id} — two captures in one path ------
    // A path may carry any number of `{name}` segments interleaved with
    // literals (the `/orders/{id}/a/{b}/c` shape). The Params struct binds all
    // of them: field names must equal the {name}s, and each field's type is the
    // document's path-parameter schema.
    server.route<OwnerPetParams, openapi::NoBody, Pet>(
        {Method::Get}, "/owners/{owner}/pets/{pet_id}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<OwnerPetParams>(*req);
            if (!p || p->owner.empty() || p->pet_id == 0) {
                co_await res->status(404).content_type(mime::app_json).send(to_json(ErrorBody{.error = "no such pet"}));
                co_return;
            }
            co_await res->status(200)
                .content_type(mime::app_json)
                .send(to_json(Pet{.id = static_cast<std::int64_t>(p->pet_id), .name = "rex", .status = "available"}));
        },
        openapi::OperationInfo{.summary = "get a pet of an owner", .tag = "pet", .operation_id = "getOwnedPet"},
        openapi::resp<ErrorBody>(404, "no such pet"));

    // --- GET /orders/{order_id}/items/{item_id} — the axum example, verbatim ---
    // The exact shape from the reference: a Params struct the route template is
    // bound to, parsed values delivered to the handler. Here via
    // req->path_params<OrderItemParams>(); the route types derive both the
    // document parameters and the handler's inputs from the same struct.
    server.route<OrderItemParams, openapi::NoBody, Order>(
        {Method::Get}, "/orders/{order_id}/items/{item_id}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<OrderItemParams>(*req);
            if (!p) {
                co_await res->status(404)
                    .content_type(mime::app_json)
                    .send(to_json(ErrorBody{.error = "no such item"}));
                co_return;
            }
            co_await res->status(200)
                .content_type(mime::app_json)
                .send(to_json(Order{.id = static_cast<std::int64_t>(p->item_id),
                                    .pet_id = static_cast<std::int64_t>(p->order_id),
                                    .quantity = 1,
                                    .status = "placed"}));
        },
        openapi::OperationInfo{.summary = "get an order item",
                               .tag = "order",
                               .description = "two captures in one path, like axum's Path<OrderItemParams>",
                               .operation_id = "getOrderItem"},
        openapi::resp<ErrorBody>(404, "no such item"));

    // --- POST /pets — request body + 201 + two error responses + security ----
    server.route<CreatePetReq, Pet>(
        {Method::Post}, "/pets",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            if (!authorized(req)) {
                co_await unauthorized(res);
                co_return;
            }
            auto in = co_await read_body_api<CreatePetReq>(req, res);
            if (!in) {
                co_return;
            }
            // The declared response header, set for
            // real: doc and wire agree on X-Request-Id.
            (void)co_await res->status(201)
                .header("x-request-id", "req-42")
                .content_type(mime::app_json)
                .send(to_json(Pet{.id = 100, .name = in->name, .status = in->status.value_or("available")}));
        },
        openapi::OperationInfo{
            .summary = "create a pet",
            .tag = "pet",
            .success_status = 201,
            .description = "Add a pet to the store.",
            .operation_id = "createPet",
            .security = {"bearer"},
            .response_headers = {{"X-Request-Id", openapi::response_header<std::string>("the request id")}}},
        openapi::resp<ErrorBody>(400, "invalid request body"), openapi::resp<ErrorBody>(409, "name already taken"));

    // --- GET /orders/{id} — a second resource ---------------------------------
    server.route<Order>(
        {Method::Get}, "/orders/{id}",
        [](const RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto id = parse_id(req, "id", 0);
            if (id <= 0) {
                co_await res->status(404)
                    .content_type(mime::app_json)
                    .send(to_json(ErrorBody{.error = "no such order"}));
                co_return;
            }
            co_await res->status(200)
                .content_type(mime::app_json)
                .send(to_json(Order{.id = id, .pet_id = 10, .quantity = 2}));
        },
        openapi::OperationInfo{.summary = "get an order", .tag = "order", .operation_id = "getOrder"},
        openapi::resp<ErrorBody>(404, "no such order"));

    // --- POST /orders — writes require the bearer token too -------------------
    server.route<CreateOrderReq, Order>(
        {Method::Post}, "/orders",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            if (!authorized(req)) {
                co_await unauthorized(res);
                co_return;
            }
            auto in = co_await read_body_api<CreateOrderReq>(req, res);
            if (!in) {
                co_return;
            }
            co_await res->status(201)
                .content_type(mime::app_json)
                .send(to_json(Order{.id = 7, .pet_id = in->pet_id, .quantity = in->quantity.value_or(1)}));
        },
        openapi::OperationInfo{.summary = "place an order",
                               .tag = "order",
                               .success_status = 201,
                               .operation_id = "placeOrder",
                               .security = {"bearer"}},
        openapi::resp<ErrorBody>(400, "invalid request body"));

    // --- mixed scalar path-parameter types -------------------------------------
    // string + double + double + unsigned integer in one Params struct: each
    // capture parses into its field type, and the handler echoes the parsed
    // values back so the client can assert the exact numbers made the round
    // trip (39.9 must come back as 39.9, not a string, not truncated).
    server.route<GeoParams, openapi::NoBody, GeoReply>(
        {Method::Get}, "/geo/{city}/points/{lat}/{lng}/{precision}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<GeoParams>(*req);
            if (!p) {
                co_await res->status(404)
                    .content_type(mime::app_json)
                    .send(to_json(ErrorBody{.error = "malformed geo coordinate"}));
                co_return;
            }
            co_await res->status(200)
                .content_type(mime::app_json)
                .send(to_json(GeoReply{.city = p->city, .lat = p->lat, .lng = p->lng, .precision = p->precision}));
        },
        openapi::OperationInfo{.summary = "resolve a geo point", .tag = "geo", .operation_id = "resolveGeo"},
        openapi::resp<ErrorBody>(404, "malformed coordinate or precision"));

    // --- a boolean path parameter
    // ------------------------------------------------- The bool parses exactly
    // "true"/"false" as JSON scalars; anything else
    // ("yes", "1") fails → nullopt → 404. A client is thereby constrained to
    // the documented values, exactly like axum's typed params.
    server.route<LabParams, openapi::NoBody, LabReply>(
        {Method::Get}, "/lab/{experiment}/{run}/{fast}",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            const auto p = openapi::path_params<LabParams>(*req);
            if (!p) {
                co_await res->status(404)
                    .content_type(mime::app_json)
                    .send(to_json(ErrorBody{.error = "malformed lab run"}));
                co_return;
            }
            co_await res->status(200)
                .content_type(mime::app_json)
                .send(to_json(LabReply{.experiment = p->experiment, .run = p->run, .fast = p->fast}));
        },
        openapi::OperationInfo{.summary = "inspect a lab run", .tag = "lab", .operation_id = "getLabRun"},
        openapi::resp<ErrorBody>(404, "malformed run or flag"));

    // --- a literal that would otherwise match a template (specificity)
    // ----------- /pets/search is a literal exact route and `search` would also
    // match the template /pets/{id}; the literal exact map is consulted first, so
    // the search endpoint wins and /pets/{id} never sees "search" as an id.
    server.route<PetList>(
        {Method::Get}, "/pets/search",
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> {
            PetList found;
            found.items.emplace_back(Pet{.id = 1, .name = "search-hit", .status = "available"});
            co_await res->status(200).content_type(mime::app_json).send(to_json(found));
        },
        openapi::OperationInfo{.summary = "search pets",
                               .tag = "pet",
                               .description = "must beat the /pets/{id} template",
                               .operation_id = "searchPets"},
        openapi::query<std::string>("q", {.description = "search term"}));

    // --- path parameters + request body + security in one route
    // ----------------- A rate-limit response that actually carries a declared
    // Retry-After header (doc only here; the handler may send it the same way the
    // 201 sets X-Request-Id above).
    openapi::Response rate_limited = openapi::resp<ErrorBody>(429, "rate limited");
    rate_limited.headers = {{"Retry-After", openapi::response_header<std::uint64_t>("seconds until you may retry")}};
    server.route<OwnerParams, CreatePetReq, Pet>(
        {Method::Post}, "/owners/{owner_id}/pets",
        [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
            if (!authorized(req)) {
                co_await unauthorized(res);
                co_return;
            }
            const auto p = openapi::path_params<OwnerParams>(*req);
            if (!p || p->owner_id == 0) {
                co_await res->status(404)
                    .content_type(mime::app_json)
                    .send(to_json(ErrorBody{.error = "no such owner"}));
                co_return;
            }
            auto in = co_await read_body_api<CreatePetReq>(req, res);
            if (!in) {
                co_return;
            }
            co_await res->status(201)
                .content_type(mime::app_json)
                .send(to_json(Pet{.id = static_cast<std::int64_t>(p->owner_id),
                                  .name = in->name,
                                  .status = in->status.value_or("available")}));
        },
        openapi::OperationInfo{.summary = "add a pet to an owner",
                               .tag = "pet",
                               .success_status = 201,
                               .description = "path param + body + security together.",
                               .operation_id = "createOwnedPet",
                               .security = {"bearer"}},
        openapi::resp<ErrorBody>(400, "invalid request body"), openapi::resp<ErrorBody>(404, "no such owner"),
        std::move(rate_limited));

    // --- a *plain* route with template captures ---------------------------------
    // No typed Params, no document entry: the base route() still treats a `{x}`
    // path as a template and the handler reads the captures with req->param.
    server.route({Method::Get}, "/echo/{what}/times/{n}",
                  [](RequestPtr req, ResponsePtr res) -> asio::awaitable<void> {
                      const auto what = req->param("what").value_or("");
                      const auto n = req->param("n").value_or("");
                      co_await res->status(200).content_type(mime::text_plain)
                          .send(std::string{"what="} + std::string{what} + " n=" + std::string{n});
                  });

    // Serve the collected document and a CDN-backed Swagger UI pointing at it.
    server.serve_openapi("/openapi.json");
    server.serve_swagger_ui("/swagger", "/openapi.json");
    server.fallback(
        [](RequestPtr, ResponsePtr res) -> asio::awaitable<void> { co_await res->status(404).send("not found"); });

    if (!server.start()) {
        std::println("bind 127.0.0.1:7795 failed");
        return 1;
    }
    std::println("petshop OpenAPI demo: http://127.0.0.1:7795");
    std::println("  document: http://127.0.0.1:7795/openapi.json");
    std::println("  swagger:  http://127.0.0.1:7795/swagger");

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}