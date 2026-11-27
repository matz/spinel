# The declarations of the C extension, one file (the gem probes for a
# precompiled extension under the running Ruby version first, by a computed
# path under a rescue, which would load this file at no fixed point of the
# load order)
require "sqlite3/sqlite3_native"

require "sqlite3/database"
require "sqlite3/version"

module SQLite3
  # Was sqlite3 compiled with thread safety on?
  def self.threadsafe?
    threadsafe > 0
  end
end

require "sqlite3/version_info"
