// Copyright 2014 MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "helpers.hpp"

#include <chrono>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/private/helpers.hh>
#include <bsoncxx/private/libbson.hh>
#include <bsoncxx/stdx/make_unique.hpp>
#include <bsoncxx/stdx/optional.hpp>
#include <bsoncxx/test_util/catch.hh>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/logic_error.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/private/conversions.hh>
#include <mongocxx/private/libbson.hh>
#include <mongocxx/private/libmongoc.hh>
#include <mongocxx/read_preference.hpp>

namespace {
using namespace mongocxx;
using namespace bsoncxx;

using builder::basic::kvp;
using builder::basic::make_document;

TEST_CASE("A default constructed collection is false-ish", "[collection]") {
    instance::current();

    collection c;
    REQUIRE(!c);
}

TEST_CASE("Collection", "[collection]") {
    instance::current();

    // dummy_collection is the name the mocked collection_get_name returns
    const std::string collection_name("dummy_collection");
    const std::string database_name("mocked_collection");

    MOCK_CLIENT
    MOCK_DATABASE
    MOCK_COLLECTION
    MOCK_FAM
    MOCK_BULK
    MOCK_CURSOR

    client mongo_client{uri{}};
    write_concern concern;
    database mongo_db = mongo_client[database_name];
    collection mongo_coll = mongo_db[collection_name];
    REQUIRE(mongo_coll);

    SECTION("Read Concern", "[collection]") {
        auto collection_set_rc_called = false;
        read_concern rc{};
        rc.acknowledge_level(read_concern::level::k_majority);

        collection_set_read_concern->interpose([&collection_set_rc_called](
            ::mongoc_collection_t*, const ::mongoc_read_concern_t* rc_t) {
            REQUIRE(rc_t);
            const auto result = libmongoc::read_concern_get_level(rc_t);
            REQUIRE(result);
            REQUIRE(strcmp(result, "majority") == 0);
            collection_set_rc_called = true;
        });

        mongo_coll.read_concern(rc);
        REQUIRE(collection_set_rc_called);
    }

    auto filter_doc = make_document(kvp("_id", "wow"), kvp("foo", "bar"));

    SECTION("Aggregate", "[Collection::aggregate]") {
        auto collection_aggregate_called = false;
        auto expected_allow_disk_use = true;
        auto expected_max_time_ms = 1234;
        auto expected_batch_size = 5678;
        auto expected_bypass_document_validation = true;
        auto expected_read_preference =
            read_preference{}.mode(read_preference::read_mode::k_secondary);

        pipeline pipe;
        options::aggregate opts;

        collection_aggregate->interpose(
            [&](mongoc_collection_t*,
                mongoc_query_flags_t flags,
                const bson_t* pipeline,
                const bson_t* options,
                const mongoc_read_prefs_t* read_preference) -> mongoc_cursor_t* {
                collection_aggregate_called = true;
                REQUIRE(flags == MONGOC_QUERY_NONE);

                bsoncxx::array::view p(bson_get_data(pipeline), pipeline->len);
                bsoncxx::document::view o(bson_get_data(options), options->len);

                bsoncxx::stdx::string_view bar(
                    p[0].get_document().value["$match"].get_document().value["foo"].get_utf8());
                std::int32_t one(
                    p[1].get_document().value["$sort"].get_document().value["foo"].get_int32());

                REQUIRE(bar == bsoncxx::stdx::string_view("bar"));
                REQUIRE(one == 1);

                if (opts.allow_disk_use())
                    REQUIRE(o["allowDiskUse"].get_bool().value == expected_allow_disk_use);
                else
                    REQUIRE(o.find("allowDiskUse") == o.end());

                if (opts.max_time())
                    REQUIRE(o["maxTimeMS"].get_int64().value == expected_max_time_ms);
                else
                    REQUIRE(o.find("maxTimeMS") == o.end());

                if (opts.bypass_document_validation())
                    REQUIRE(o["bypassDocumentValidation"].get_bool().value ==
                            expected_bypass_document_validation);
                else
                    REQUIRE(!o["bypassDocumentValidation"]);

                if (opts.read_preference())
                    REQUIRE(mongoc_read_prefs_get_mode(read_preference) ==
                            static_cast<int>(opts.read_preference()->mode()));
                else
                    REQUIRE(mongoc_read_prefs_get_mode(read_preference) ==
                            libmongoc::conversions::read_mode_t_from_read_mode(
                                mongo_coll.read_preference().mode()));

                if (opts.batch_size())
                    REQUIRE(o["batchSize"].get_int32().value == expected_batch_size);
                else
                    REQUIRE(o.find("batchSize") == o.end());

                return NULL;
            });

        pipe.match(make_document(kvp("foo", "bar")));
        pipe.sort(make_document(kvp("foo", 1)));

        SECTION("With default options") {}

        SECTION("With some options") {
            opts.allow_disk_use(expected_allow_disk_use);
            opts.max_time(std::chrono::milliseconds{expected_max_time_ms});
            opts.batch_size(expected_batch_size);
            opts.bypass_document_validation(expected_bypass_document_validation);
            opts.read_preference(expected_read_preference);
        }

        mongo_coll.aggregate(pipe, opts);

        REQUIRE(collection_aggregate_called);
    }

    SECTION("Count", "[collection::count]") {
        auto collection_count_called = false;
        bool success = true;
        std::int64_t expected_skip = 0;
        std::int64_t expected_limit = 0;

        const bson_t* expected_opts = nullptr;

        collection_count_with_opts->interpose([&](mongoc_collection_t*,
                                                  mongoc_query_flags_t flags,
                                                  const bson_t* query,
                                                  int64_t skip,
                                                  int64_t limit,
                                                  const bson_t* cmd_opts,
                                                  const mongoc_read_prefs_t*,
                                                  bson_error_t* error) {
            collection_count_called = true;
            REQUIRE(flags == MONGOC_QUERY_NONE);
            REQUIRE(bson_get_data(query) == filter_doc.view().data());
            REQUIRE(skip == expected_skip);
            REQUIRE(limit == expected_limit);
            if (expected_opts) {
                REQUIRE(bson_equal(cmd_opts, expected_opts));
            }

            if (success)
                return 123;

            // The caller expects the bson_error_t to have been
            // initialized by the call to count in the event of an
            // error.
            bson_set_error(error,
                           MONGOC_ERROR_COMMAND,
                           MONGOC_ERROR_COMMAND_INVALID_ARG,
                           "expected error from mock");

            return -1;
        });

        SECTION("Succeeds with defaults") {
            REQUIRE_NOTHROW(mongo_coll.count(filter_doc.view()));
        }

        SECTION("Succeeds with options") {
            options::count opts;
            opts.skip(expected_skip);
            opts.limit(expected_limit);
            REQUIRE_NOTHROW(mongo_coll.count(filter_doc.view(), opts));
        }

        SECTION("Succeeds with hint") {
            options::count opts;
            hint index_hint("a_1");
            opts.hint(index_hint);

            // set our expected_opts so we check against that
            bsoncxx::document::value doc = make_document(kvp("hint", index_hint.to_value()));
            libbson::scoped_bson_t cmd_opts{std::move(doc)};
            expected_opts = cmd_opts.bson();

            REQUIRE_NOTHROW(mongo_coll.count(filter_doc.view(), opts));
        }

        SECTION("Succeeds with read_prefs") {
            options::count opts;
            read_preference rp;
            rp.mode(read_preference::read_mode::k_secondary);
            opts.read_preference(rp);
            REQUIRE_NOTHROW(mongo_coll.count(filter_doc.view(), opts));
        }

        SECTION("Fails") {
            success = false;
            REQUIRE_THROWS_AS(mongo_coll.count(filter_doc.view()), operation_exception);
        }

        REQUIRE(collection_count_called);
    }

    SECTION("Find", "[collection::find]") {
        auto collection_find_called = false;
        auto find_doc = make_document(kvp("a", 1));
        auto doc = find_doc.view();
        mongocxx::stdx::optional<bool> expected_allow_partial_results;
        mongocxx::stdx::optional<bsoncxx::stdx::string_view> expected_comment{};
        mongocxx::stdx::optional<mongocxx::cursor::type> expected_cursor_type{};
        mongocxx::stdx::optional<bsoncxx::types::value> expected_hint{};
        mongocxx::stdx::optional<bool> expected_no_cursor_timeout;
        mongocxx::stdx::optional<bsoncxx::document::view> expected_sort{};
        mongocxx::stdx::optional<read_preference> expected_read_preference{};

        collection_find_with_opts->interpose([&](mongoc_collection_t*,
                                                 const bson_t* filter,
                                                 const bson_t* opts,
                                                 const mongoc_read_prefs_t* read_prefs) {
            collection_find_called = true;

            bsoncxx::document::view filter_view{bson_get_data(filter), filter->len};
            bsoncxx::document::view opts_view{bson_get_data(opts), opts->len};

            REQUIRE(filter_view == doc);

            if (expected_allow_partial_results) {
                REQUIRE(opts_view["allowPartialResults"].get_bool().value ==
                        *expected_allow_partial_results);
            }
            if (expected_comment) {
                REQUIRE(opts_view["comment"].get_utf8().value == *expected_comment);
            }
            if (expected_cursor_type) {
                bsoncxx::document::element tailable = opts_view["tailable"];
                bsoncxx::document::element awaitData = opts_view["awaitData"];
                switch (*expected_cursor_type) {
                    case mongocxx::cursor::type::k_non_tailable:
                        REQUIRE(!tailable);
                        REQUIRE(!awaitData);
                        break;
                    case mongocxx::cursor::type::k_tailable:
                        REQUIRE(tailable.get_bool().value);
                        REQUIRE(!awaitData);
                        break;
                    case mongocxx::cursor::type::k_tailable_await:
                        REQUIRE(tailable.get_bool().value);
                        REQUIRE(awaitData.get_bool().value);
                        break;
                }
            }
            if (expected_hint) {
                REQUIRE(opts_view["hint"].get_utf8() == expected_hint->get_utf8());
            }
            if (expected_no_cursor_timeout) {
                REQUIRE(opts_view["noCursorTimeout"].get_bool().value ==
                        *expected_no_cursor_timeout);
            }
            if (expected_sort) {
                REQUIRE(opts_view["sort"].get_document() == *expected_sort);
            }

            if (expected_read_preference)
                REQUIRE(mongoc_read_prefs_get_mode(read_prefs) ==
                        static_cast<int>(expected_read_preference->mode()));
            else
                REQUIRE(mongoc_read_prefs_get_mode(read_prefs) ==
                        libmongoc::conversions::read_mode_t_from_read_mode(
                            mongo_coll.read_preference().mode()));

            mongoc_cursor_t* cursor = NULL;
            return cursor;
        });

        SECTION("find succeeds") {
            REQUIRE_NOTHROW(mongo_coll.find(doc));
        }

        SECTION("Succeeds with allow_partial_results") {
            options::find opts;
            expected_allow_partial_results = true;
            opts.allow_partial_results(*expected_allow_partial_results);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with comment") {
            expected_comment.emplace("my comment");
            options::find opts;
            opts.comment(*expected_comment);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with cursor type") {
            options::find opts;
            expected_cursor_type = mongocxx::cursor::type::k_tailable;
            opts.cursor_type(*expected_cursor_type);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with hint") {
            options::find opts;
            hint index_hint("a_1");
            expected_hint = index_hint.to_value();
            opts.hint(index_hint);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with no_cursor_timeout") {
            options::find opts;
            expected_no_cursor_timeout = true;
            opts.no_cursor_timeout(*expected_no_cursor_timeout);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with sort") {
            options::find opts{};
            auto sort_doc = make_document(kvp("x", -1));
            expected_sort = sort_doc.view();
            opts.sort(*expected_sort);
            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        SECTION("Succeeds with read preference") {
            options::find opts{};
            expected_read_preference.emplace();
            expected_read_preference->mode(read_preference::read_mode::k_secondary);
            opts.read_preference(*expected_read_preference);

            REQUIRE_NOTHROW(mongo_coll.find(doc, opts));
        }

        REQUIRE(collection_find_called);
    }

    SECTION("Writes", "[collection::writes]") {
        auto expected_order_setting = false;
        auto expect_set_bypass_document_validation_called = false;
        auto expected_bypass_document_validation = false;

        auto modification_doc = make_document(kvp("cool", "wow"), kvp("foo", "bar"));

        collection_create_bulk_operation_with_opts->interpose(
            [&](mongoc_collection_t*, const bson_t* opts) -> mongoc_bulk_operation_t* {
                bson_iter_t iter;
                if (bson_iter_init_find(&iter, opts, "ordered")) {
                    REQUIRE(BSON_ITER_HOLDS_BOOL(&iter));
                    REQUIRE(bson_iter_bool(&iter) == expected_order_setting);
                } else {
                    // No "ordered" in opts, default of true must equal the expected setting.
                    REQUIRE(expected_order_setting);
                }
                collection_create_bulk_operation_called = true;
                return nullptr;
            });

        bulk_operation_set_bypass_document_validation->interpose(
            [&](mongoc_bulk_operation_t*, bool bypass) {
                bulk_operation_set_bypass_document_validation_called = true;
                REQUIRE(expected_bypass_document_validation == bypass);
            });

        bulk_operation_execute->interpose(
            [&](mongoc_bulk_operation_t*, bson_t* reply, bson_error_t*) {
                bulk_operation_execute_called = true;
                bson_init(reply);
                return 1;
            });

        bulk_operation_destroy->interpose(
            [&](mongoc_bulk_operation_t*) { bulk_operation_destroy_called = true; });

        SECTION("Insert One", "[collection::insert_one]") {
            bulk_operation_insert->interpose([&](mongoc_bulk_operation_t*, const bson_t* doc) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(doc) == filter_doc.view().data());
            });

            mongo_coll.insert_one(filter_doc.view());
        }

        SECTION("Insert One Bypassing Validation", "[collection::insert_one]") {
            bulk_operation_insert->interpose([&](mongoc_bulk_operation_t*, const bson_t* doc) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(doc) == filter_doc.view().data());
            });

            expect_set_bypass_document_validation_called = true;
            SECTION("...set to false") {
                expected_bypass_document_validation = false;
            }
            SECTION("...set to true") {
                expected_bypass_document_validation = true;
            }
            options::insert opts{};
            opts.bypass_document_validation(expected_bypass_document_validation);
            mongo_coll.insert_one(filter_doc.view(), opts);
        }

        SECTION("Insert Many Ordered", "[collection::insert_many]") {
            bulk_operation_insert->interpose([&](mongoc_bulk_operation_t*, const bson_t* doc) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(doc) == filter_doc.view().data());
            });

            // The interposed collection_create_bulk_operation_with_opts validates this setting.
            SECTION("...set to false") {
                expected_order_setting = false;
            }
            SECTION("...set to true") {
                expected_order_setting = true;
            }
            options::insert opts{};
            opts.ordered(expected_order_setting);
            std::vector<bsoncxx::document::view> docs{};
            docs.push_back(filter_doc.view());
            mongo_coll.insert_many(docs, opts);
        }

        SECTION("Update One", "[collection::update_one]") {
            bool upsert_option = false;

            bulk_operation_update_one_with_opts->interpose([&](mongoc_bulk_operation_t*,
                                                               const bson_t* query,
                                                               const bson_t* update,
                                                               const bson_t* options,
                                                               bson_error_t*) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(query) == filter_doc.view().data());
                REQUIRE(bson_get_data(update) == modification_doc.view().data());

                bsoncxx::document::view options_view{bson_get_data(options), options->len};

                bsoncxx::document::element upsert = options_view["upsert"];
                if (upsert_option) {
                    REQUIRE(upsert);
                    REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                    REQUIRE(upsert.get_bool().value);
                } else {
                    // Allow either no "upsert" option, or an "upsert" option set to false.
                    if (upsert) {
                        REQUIRE(upsert);
                        REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                        REQUIRE(!upsert.get_bool().value);
                    }
                }

                return true;
            });

            options::update options;

            SECTION("Default Options") {}

            SECTION("Upsert true") {
                upsert_option = true;
                options.upsert(upsert_option);
            }

            SECTION("Upsert false") {
                upsert_option = false;
                options.upsert(upsert_option);
            }

            SECTION("With bypass_document_validation") {
                expect_set_bypass_document_validation_called = true;
                expected_bypass_document_validation = true;
                options.bypass_document_validation(expected_bypass_document_validation);
            }

            SECTION("Write Concern provided") {
                options.write_concern(concern);
            }

            mongo_coll.update_one(filter_doc.view(), modification_doc.view(), options);
        }

        SECTION("Update Many", "[collection::update_many]") {
            bool upsert_option;

            bulk_operation_update_many_with_opts->interpose([&](mongoc_bulk_operation_t*,
                                                                const bson_t* query,
                                                                const bson_t* update,
                                                                const bson_t* options,
                                                                bson_error_t*) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(query) == filter_doc.view().data());
                REQUIRE(bson_get_data(update) == modification_doc.view().data());

                bsoncxx::document::view options_view{bson_get_data(options), options->len};

                bsoncxx::document::element upsert = options_view["upsert"];
                if (upsert_option) {
                    REQUIRE(upsert);
                    REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                    REQUIRE(upsert.get_bool().value);
                } else {
                    // Allow either no "upsert" option, or an "upsert" option set to false.
                    if (upsert) {
                        REQUIRE(upsert);
                        REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                        REQUIRE(!upsert.get_bool().value);
                    }
                }

                return true;
            });

            options::update options;

            SECTION("Default Options") {
                upsert_option = false;
            }

            SECTION("Upsert true") {
                upsert_option = true;
                options.upsert(upsert_option);
            }

            SECTION("Upsert false") {
                upsert_option = false;
                options.upsert(upsert_option);
            }

            mongo_coll.update_many(filter_doc.view(), modification_doc.view(), options);
        }

        SECTION("Replace One", "[collection::replace_one]") {
            bool upsert_option;

            bulk_operation_replace_one_with_opts->interpose([&](mongoc_bulk_operation_t*,
                                                                const bson_t* query,
                                                                const bson_t* update,
                                                                const bson_t* options,
                                                                bson_error_t*) {
                bulk_operation_op_called = true;
                REQUIRE(bson_get_data(query) == filter_doc.view().data());
                REQUIRE(bson_get_data(update) == modification_doc.view().data());

                bsoncxx::document::view options_view{bson_get_data(options), options->len};

                bsoncxx::document::element upsert = options_view["upsert"];
                if (upsert_option) {
                    REQUIRE(upsert);
                    REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                    REQUIRE(upsert.get_bool().value);
                } else {
                    // Allow either no "upsert" option, or an "upsert" option set to false.
                    if (upsert) {
                        REQUIRE(upsert);
                        REQUIRE(upsert.type() == bsoncxx::type::k_bool);
                        REQUIRE(!upsert.get_bool().value);
                    }
                }

                return true;
            });

            options::update options;

            SECTION("Default Options") {
                upsert_option = false;
            }

            SECTION("Upsert true") {
                upsert_option = true;
                options.upsert(upsert_option);
            }

            SECTION("Upsert false") {
                upsert_option = false;
                options.upsert(upsert_option);
            }

            mongo_coll.replace_one(filter_doc.view(), modification_doc.view(), options);
        }

        SECTION("Delete One", "[collection::delete_one]") {
            bulk_operation_remove_one_with_opts->interpose(
                [&](mongoc_bulk_operation_t*, const bson_t* doc, const bson_t*, bson_error_t*) {
                    bulk_operation_op_called = true;
                    REQUIRE(bson_get_data(doc) == filter_doc.view().data());
                    return true;
                });

            mongo_coll.delete_one(filter_doc.view());
        }

        SECTION("Delete Many", "[collection::delete_many]") {
            bulk_operation_remove_many_with_opts->interpose(
                [&](mongoc_bulk_operation_t*, const bson_t* doc, const bson_t*, bson_error_t*) {
                    bulk_operation_op_called = true;
                    REQUIRE(bson_get_data(doc) == filter_doc.view().data());
                    return true;
                });

            mongo_coll.delete_many(filter_doc.view());
        }

        REQUIRE(collection_create_bulk_operation_called);
        REQUIRE(expect_set_bypass_document_validation_called ==
                bulk_operation_set_bypass_document_validation_called);
        REQUIRE(bulk_operation_op_called);
        REQUIRE(bulk_operation_execute_called);
        REQUIRE(bulk_operation_destroy_called);
    }
}
}  // namespace
