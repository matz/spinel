# A capitalized literal may name a constant a macro binds (`const_set(:Hash,
# ..)`), so a class body under that name is not followed. The builtins
# spliced ahead of the program bind no constant: their literals (the "Hash"
# of Enumerable#tally's TypeError message) leave a `module Hash` followed.
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

p [1, 2, 1].tally
p Lib::Hash::BYTES
p Lib::Hash.crypto_hash_bytes
