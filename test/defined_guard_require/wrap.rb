begin
  require_relative "lib" unless defined?(GuardLib)
rescue LoadError
  exit 1
end

module GuardUser
  module GuardLib
    def self.tag = :nested
  end
end
