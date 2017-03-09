//
//  c4PerfTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/20/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Fleece.h"     // including this before c4 makes FLSlice and C4Slice compatible
#include "c4Test.hh"
#include "c4Document+Fleece.h"
#include "Base.hh"
#include "Benchmark.hh"
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <thread>
#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace fleece;


class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation) { }

    // Copies a Fleece dictionary key/value to an encoder
    static bool copyValue(Dict srcDict, Dict::Key &key, Encoder &enc) {
        Value value = srcDict[key];
        if (!value)
            return false;
        enc.writeKey(key);
        enc.writeValue(value);
        return true;
    }


    unsigned insertDocs(Array docs) {
        Dict::Key typeKey   (FLSTR("Track Type"), true);
        Dict::Key idKey     (FLSTR("Persistent ID"), true);
        Dict::Key nameKey   (FLSTR("Name"), true);
        Dict::Key albumKey  (FLSTR("Album"), true);
        Dict::Key artistKey (FLSTR("Artist"), true);
        Dict::Key timeKey   (FLSTR("Total Time"), true);
        Dict::Key genreKey  (FLSTR("Genre"), true);
        Dict::Key yearKey   (FLSTR("Year"), true);
        Dict::Key trackNoKey(FLSTR("Track Number"), true);
        Dict::Key compKey   (FLSTR("Compilation"), true);

        TransactionHelper t(db);

        Encoder enc;
        unsigned numDocs = 0;
        for (Value item : docs) {
            // Check that track is correct type:
            Dict track = item.asDict();

            FLSlice trackType = track.get(typeKey).asString();
            if (trackType != FLSTR("File") && trackType != FLSTR("Remote"))
                continue;

            FLSlice trackID = track.get(idKey).asString();
            REQUIRE(trackID.buf);

            // Encode doc body:
            enc.beginDict();
            REQUIRE(copyValue(track, nameKey, enc));
            copyValue(track, albumKey, enc);
            copyValue(track, artistKey, enc);
            copyValue(track, timeKey, enc);
            copyValue(track, genreKey, enc);
            copyValue(track, yearKey, enc);
            copyValue(track, trackNoKey, enc);
            copyValue(track, compKey, enc);
            enc.endDict();
            FLError error;
            FLSliceResult body = enc.finish(&error);
            REQUIRE(body.buf);
            enc.reset();

            // Save document:
            C4Error c4err;
            C4DocPutRequest rq = {};
            rq.docID = trackID;
            rq.body = (C4Slice)body;
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_free(doc);
            FLSliceResult_Free(body);
            ++numDocs;
        }
        
        return numDocs;
    }


    unsigned queryWhere(const char *whereStr, bool verbose =false) {
        std::vector<std::string> docIDs;
        docIDs.reserve(1200);

        C4Error error;
        C4Query *query = c4query_new(db, c4str(whereStr), &error);
        REQUIRE(query);
        auto e = c4query_run(query, nullptr, kC4SliceNull, &error);
        C4SliceResult artistSlice;
        while (c4queryenum_next(e, &error)) {
            std::string artist((const char*)e->docID.buf, e->docID.size);
            if (verbose) std::cerr << artist << "  ";
            docIDs.push_back(artist);
        }
        c4queryenum_free(e);
        c4query_free(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) docIDs.size();
    }

};


N_WAY_TEST_CASE_METHOD(PerfTest, "Performance", "[Perf][C]") {
    auto jsonData = readFile(sFixturesDir + "iTunesMusicLibrary.json");
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    free((void*)jsonData.buf);
    Array root = FLValue_AsArray(FLValue_FromTrustedData((C4Slice)fleeceData));
    unsigned numDocs;

    {
        Stopwatch st;
        numDocs = insertDocs(root);
        CHECK(numDocs == 12189);
        st.printReport("Writing docs", numDocs, "doc");
    }
    FLSliceResult_Free(fleeceData);
}


N_WAY_TEST_CASE_METHOD(PerfTest, "Import geoblocks", "[Perf][C][.slow]") {
    // Download https://github.com/arangodb/example-datasets/raw/master/IPRanges/geoblocks.json
    // to C/tests/data/ before running this test.
    //
    // Docs look like:
    // { "locId" : 17, "endIpNum" : 16777471, "startIpNum" : 16777216, "geo" : [ -27, 133 ] }

    auto numDocs = importJSONLines(sFixturesDir + "geoblocks.json", 15.0, true);
    reopenDB();
    {
        Stopwatch st;
        auto readNo = 0;
        for (; readNo < 100000; ++readNo) {
            char docID[30];
            sprintf(docID, "%07u", ((unsigned)random() % numDocs) + 1);
            C4Error error;
            auto doc = c4doc_get(db, c4str(docID), true, &error);
            REQUIRE(doc);
            REQUIRE(doc->selectedRev.body.size > 10);
            c4doc_free(doc);
        }
        st.printReport("Reading random docs", readNo, "doc");
    }
	std::this_thread::sleep_for(std::chrono::seconds(1)); //TEMP
}

N_WAY_TEST_CASE_METHOD(PerfTest, "Import names", "[Perf][C][.slow]") {
    // Download https://github.com/arangodb/example-datasets/raw/master/RandomUsers/names_300000.json
    // to C/tests/data/ before running this test.
    //
    // Docs look like:
    // {"name":{"first":"Travis","last":"Mutchler"},"gender":"female","birthday":"1990-12-21","contact":{"address":{"street":"22 Kansas Cir","zip":"45384","city":"Wilberforce","state":"OH"},"email":["Travis.Mutchler@nosql-matters.org","Travis@nosql-matters.org"],"region":"937","phone":["937-3512486"]},"likes":["travelling"],"memberSince":"2010-01-01"}

    auto numDocs = importJSONLines(sFixturesDir + "names_300000.json", 15.0, true);
    const bool complete = (numDocs == 300000);
#ifdef NDEBUG
    REQUIRE(numDocs == 300000);
#endif
    for (int pass = 0; pass < 2; ++pass) {
        Stopwatch st;
        auto n = queryWhere("{\"contact.address.state\": \"WA\"}");
        st.printReport("SQL query of state", n, "doc");
        if (complete) CHECK(n == 5053);
        if (pass == 0) {
            Stopwatch st2;
            C4Error error;
            REQUIRE(c4db_createIndex(db, C4STR("contact.address.state"), kC4ValueIndex, nullptr, &error));
            st2.printReport("Creating SQL index of state", 1, "index");
        }
    }
}
