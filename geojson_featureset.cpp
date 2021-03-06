/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2011 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/
// mapnik
#include <mapnik/feature_factory.hpp>
#include <mapnik/geometry.hpp>
#include <mapnik/util/geometry_to_wkt.hpp>

#include <string>
#include <vector>

// yajl
#include "yajl/yajl_parse.h"
#include "geojson_featureset.hpp"

#ifdef MAPNIK_DEBUG
#include <mapnik/timer.hpp>
#include <iomanip>
#include <sstream>
#endif

mapnik::transcoder* tr = new mapnik::transcoder("utf-8");

static int gj_start_map(void * ctx) {
    return 1;
}

static int gj_map_key(void * ctx, const unsigned char* key, size_t t) {
    std::string key_ = std::string((const char*) key, t);
    pstate *cs = static_cast<pstate*>(ctx);
    if (cs->state == parser_in_properties) {
        cs->property_name = key_;
    } else {
        if (key_ == "features") {
            cs->state = parser_in_features;
        } else if (key_ == "geometry") {
            cs->state = parser_in_geometry;
        } else if ((cs->state == parser_in_geometry) &&
            (key_ == "type")) {
            cs->state = parser_in_type;
        } else if (key_ == "properties") {
            cs->state = parser_in_properties;
        } else if (key_ == "coordinates") {
            cs->state = parser_in_coordinates;
        }
    }
    return 1;
}

static int gj_end_map(void * ctx) {
    pstate *cs = static_cast<pstate*>(ctx);

    if ((cs->state == parser_in_properties) ||
        (cs->state == parser_in_geometry)) {
        cs->state = parser_in_feature;
    } else if (cs->state == parser_in_feature) {
        cs->state = parser_in_features;
        cs->done = 1;
    }
    return 1;
}

static int gj_null(void * ctx) {
    pstate *cs = static_cast<pstate*>(ctx);
    if (cs->state == parser_in_properties) {
        boost::put(*(cs->feature),
            cs->property_name, mapnik::value_null());
    }
    return 1;
}

static int gj_boolean(void * ctx, int x) {
    pstate *cs = static_cast<pstate*>(ctx);
    if (cs->state == parser_in_properties) {
        boost::put(*(cs->feature), cs->property_name, x);
    }
    return 1;
}

static int gj_number(void * ctx, const char* str, size_t t) {
    pstate *cs = static_cast<pstate*>(ctx);
    double x = strtod(str, NULL);

    if (cs->state == parser_in_coordinates) {
        cs->point_cache.push_back(x);
    } else if (cs->state == parser_in_properties) {
        boost::put(*(cs->feature), cs->property_name, x);
    }
    return 1;
}

static int gj_string(void * ctx, const unsigned char* str, size_t t) {
    pstate *cs = static_cast<pstate*>(ctx);
    std::string str_ = std::string((const char*) str, t);
    if (cs->state == parser_in_type) {
        // TODO: change geometry type at this point
        cs->geometry_type = str_;
    } else if (cs->state == parser_in_properties) {
        UnicodeString ustr = tr->transcode(str_.c_str());
        boost::put(*(cs->feature), cs->property_name, ustr);
    }
    return 1;
}

static int gj_start_array(void * ctx) {
    pstate *cs = static_cast<pstate*>(ctx);
    if (cs->state == parser_in_coordinates) {
        cs->coord_dimensions++;
    }
    return 1;
}

static int gj_end_array(void * ctx) {
    pstate *cs = static_cast<pstate*>(ctx);
    if (cs->state == parser_in_coordinates) {
        cs->coord_dimensions--;
        cs->feature->get_geometry(0).move_to(
            cs->point_cache.at(0),
            cs->point_cache.at(1));
        if (cs->coord_dimensions < 1) {
            cs->state = parser_in_geometry;
        }
    } else if (cs->state == parser_in_features) {
        cs->state = parser_outside;
    }
    return 1;
}

static yajl_callbacks callbacks = {
    gj_null,
    gj_boolean,
    NULL,
    NULL,
    gj_number,
    gj_string,
    gj_start_map,
    gj_map_key,
    gj_end_map,
    gj_start_array,
    gj_end_array
};

geojson_featureset::geojson_featureset(
    mapnik::box2d<double> const& box,
    std::string input_string,
    std::string const& encoding)
    : box_(box),
      feature_id_(1),
      input_string_(input_string),
      tr_(new mapnik::transcoder(encoding)),
      features_(),
      itt_(),
      hand() {

    struct pstate state_bundle;

    state_bundle.state = parser_outside;
    state_bundle.done = 0;

    hand = yajl_alloc(
        &callbacks, NULL,
        &state_bundle);

    yajl_config(hand, yajl_allow_comments, 1);
    yajl_config(hand, yajl_allow_trailing_garbage, 1);

    mapnik::feature_ptr feature(mapnik::feature_factory::create(feature_id_));
    // This is just a stand-in geometry whose type can be changed later.
    mapnik::geometry_type * pt;
    pt = new mapnik::geometry_type(mapnik::LineString);
    feature->add_geometry(pt);
    state_bundle.feature = feature;

    int parse_result;
    for (itt_ = 0; itt_ < input_string_.length(); itt_++) {
        parse_result = yajl_parse(hand,
                (const unsigned char*) &input_string_[itt_], 1);

        if (parse_result == yajl_status_error) {
            unsigned char *str = yajl_get_error(hand,
                1,  (const unsigned char*) input_string_.c_str(),
                input_string_.length());
            std::ostringstream errmsg;
            errmsg << "GeoJSON Plugin: invalid GeoJSON detected: " << (const char*) str << "\n";
            yajl_free_error(hand, str);
            throw mapnik::datasource_exception(errmsg.str());
        } else if (state_bundle.done == 1) {
            features_.push_back(state_bundle.feature);

            feature_id_++;
            mapnik::feature_ptr
              feature(mapnik::feature_factory::create(feature_id_));

            // This is just a stand-in geometry whose type can be changed later.
            mapnik::geometry_type * pt;
            pt = new mapnik::geometry_type(mapnik::LineString);
            feature->add_geometry(pt);

            // reset
            state_bundle.point_cache.clear();
            state_bundle.done = 0;
            state_bundle.geometry_type = "";
            state_bundle.feature = feature;
        }
    }

    yajl_free(hand);
    feature_id_ = 0;
}

geojson_featureset::~geojson_featureset() { }

mapnik::feature_ptr geojson_featureset::next() {
    feature_id_++;
    if (feature_id_ <= features_.size()) {
        return features_.at(feature_id_ - 1);
    } else {
        return mapnik::feature_ptr();
    }
}
