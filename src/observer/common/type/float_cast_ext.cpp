/* Extends FloatType with cast_to implementations. */
#include "common/type/float_type.h"
#include "common/value.h"
#include "common/lang/sstream.h"
#include "common/lang/string.h"
#include <cmath>

RC FloatType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::FLOATS: {
      result = val;
      return RC::SUCCESS;
    }
    case AttrType::INTS: {
      // 四舍五入到最接近的整数，保证与题目期望一致
      float float_value = val.get_float();
      int   int_value   = static_cast<int>(std::round(float_value));
      result.set_int(int_value);
      return RC::SUCCESS;
    }
    case AttrType::CHARS: {
      std::string s;
      RC rc = to_string(val, s);
      if (OB_FAIL(rc)) return rc;
      result.set_string(s.c_str());
      return RC::SUCCESS;
    }
    case AttrType::TEXTS: {
      std::string s;
      RC rc = to_string(val, s);
      if (OB_FAIL(rc)) return rc;
      result.set_string(s.c_str());
      result.set_type(AttrType::TEXTS);
      return RC::SUCCESS;
    }
    default: {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
}
