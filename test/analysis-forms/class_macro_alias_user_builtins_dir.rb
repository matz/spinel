# The text pass refuses this call
# (test/reject/macro_alias_user_builtins_dir.rb); the class-body macro form
# runs the macro at the call's point under the load graph, as CRuby runs it.
require_relative "macro_alias_user_builtins_dir/builtins/names"
module Lib
  module Sodium
    def sodium_type(type = nil)
      return @type if type.nil?
      @type = type
    end

    def sodium_constant(constant, value)
      fn_name = ["crypto", sodium_type, constant.to_s.downcase].join("_")
      define_singleton_method fn_name, -> { value }
      const_set(constant, public_send(fn_name))
    end
  end

  module Hash
    extend Sodium
    sodium_type :hash
    sodium_constant :BYTES, 64
  end
end
p Lib::Hash::BYTES, NAMES
