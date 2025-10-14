#include "gtest/gtest.h"
#include "storage/index/index_meta.h"
#include "storage/field/field_meta.h"
#include "storage/table/table_meta.h"
#include "common/types.h"
#include "sql/parser/parse_defs.h"
#include "json/json.h"

TEST(VectorIndexMetaTest, SetAndSerializeVectorOptions)
{
  FieldMeta field_meta;
  ASSERT_EQ(RC::SUCCESS, field_meta.init("embedding", AttrType::VECTORS, 0, static_cast<int>(sizeof(float) * 4), true, 0));

  std::vector<FieldMeta> fields;
  fields.push_back(field_meta);

  IndexMeta index_meta;
  ASSERT_EQ(RC::SUCCESS,
            index_meta.init("vec_idx", span<const FieldMeta>(fields.data(), fields.size()), false));

  index_meta.set_vector_options(true, "IVFFLAT", "L2_DISTANCE", 245, 5);
  EXPECT_TRUE(index_meta.is_vector_index());
  EXPECT_EQ("IVFFLAT", index_meta.index_type());
  EXPECT_EQ("L2_DISTANCE", index_meta.distance_type());
  EXPECT_EQ(245, index_meta.lists());
  EXPECT_EQ(5, index_meta.probes());

  Json::Value json_value;
  index_meta.to_json(json_value);
  EXPECT_TRUE(json_value.isMember("is_vector_index"));
  EXPECT_TRUE(json_value["is_vector_index"].asBool());
  EXPECT_EQ(245, json_value["lists"].asInt());
  EXPECT_EQ(5, json_value["probes"].asInt());
  EXPECT_STREQ("IVFFLAT", json_value["index_type"].asCString());
  EXPECT_STREQ("L2_DISTANCE", json_value["distance_type"].asCString());

  AttrInfoSqlNode attr_info;
  attr_info.name   = "embedding";
  attr_info.type   = AttrType::VECTORS;
  attr_info.length = 4; // dimension
  attr_info.nullable = false;

  std::vector<AttrInfoSqlNode> attr_infos;
  attr_infos.push_back(attr_info);
  std::vector<std::string> primary_keys;

  TableMeta table_meta;
  ASSERT_EQ(RC::SUCCESS,
            table_meta.init(1,
                             "test_table",
                             nullptr,
                             span<const AttrInfoSqlNode>(attr_infos.data(), attr_infos.size()),
                             primary_keys,
                             StorageFormat::ROW_FORMAT,
                             StorageEngine::HEAP));

  IndexMeta parsed_index_meta;
  ASSERT_EQ(RC::SUCCESS, IndexMeta::from_json(table_meta, json_value, parsed_index_meta));
  EXPECT_TRUE(parsed_index_meta.is_vector_index());
  EXPECT_EQ("IVFFLAT", parsed_index_meta.index_type());
  EXPECT_EQ("L2_DISTANCE", parsed_index_meta.distance_type());
  EXPECT_EQ(245, parsed_index_meta.lists());
  EXPECT_EQ(5, parsed_index_meta.probes());
}
