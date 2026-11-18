# A file-level `K.extend(M)` / `K.include(M)` runs M's hook on K, as the
# statement in K's body does: version_gem pins a version module with
# `Foo::Version.extend(VersionGem::Basic)`, whose `extended` hook extends
# the base with Api.
module Api
  def major = self::VERSION.split(".").first
end

module Basic
  class << self
    def extended(base)
      base.extend(Api)
    end
  end
end

module Version
  VERSION = "7.1.0"
end

Version.extend(Basic)

module Plug
  module ClassMethods
    def plugged = "plugged #{name}"
  end

  def self.included(base)
    base.extend(ClassMethods)
  end
end

class Thing
end

Thing.include(Plug)

# A name the receiver's namespace also holds: the call reads it from the top level.
module Outer
  module Api
    def major = "outer"
  end

  module Edition
    VERSION = "2.0.0"
  end
end

Outer::Edition.extend(Basic)

# A namesake class elsewhere: the receiver's own declaration, a module, is the one reopened.
module Shelf
  class K
  end
end

module Rack
  module K
    VERSION = "3.0.0"
  end
end

Rack::K.extend(Basic)

# A hook defined after the call is not the call's: Ruby runs none.
module Late
end

module Version2
  VERSION = "9.0.0"
end

Version2.extend(Late)

module Late
  def self.extended(base)
    base.extend(Api)
  end
end

p Version.major
p Thing.plugged
p Outer::Edition.major
p Rack::K.major
p Version2.respond_to?(:major)
