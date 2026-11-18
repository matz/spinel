# A file-level `K.extend(M)` whose M, and its hook, come from a required
# file (version_gem's layout): the hook runs on K.
require_relative "fixtures/toplevel_mixin_hook/basic"

module Version
  VERSION = "4.2.0"
end

Version.extend(Basic)

p Version.major
