// FM1 Rack (tools/mi_build.py): plaits/user_data.h without user data. The rack has no flash page for Plaits' user
// wavetables and banks: every engine plays its factory data (UserData::ptr is always NULL, as in Mutable's TEST build)
#ifndef PLAITS_USER_DATA_H_
#define PLAITS_USER_DATA_H_
#include "stmlib/stmlib.h"
namespace plaits {
class UserData {
 public:
  enum { ADDRESS = 0, SIZE = 0x1000 };
  UserData() { }
  ~UserData() { }
  inline const uint8_t* ptr(int slot) const { (void)slot; return NULL; }
  inline bool Save(uint8_t* rx_buffer, int slot) { (void)rx_buffer; (void)slot; return false; }
};
}  // namespace plaits
#endif  // PLAITS_USER_DATA_H_
