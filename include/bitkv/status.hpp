#pragma once

namespace bitkv {

enum class Status {
  Ok,
  NotFound,
  InvalidArgument,
  IoError,
};

}  // namespace bitkv
