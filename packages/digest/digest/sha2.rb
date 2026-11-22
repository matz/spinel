# Spinel bundled `digest/sha2`: CRuby's per-algorithm spelling of the
# require, plus the Digest::SHA2 wrapper over SHA256, SHA384, and SHA512 (#4828).
require "digest"

module Digest
  class SHA2 < Digest::Class
    def initialize(bitlen = 256)
      @sha2 =
        case bitlen
        when 256 then Digest::SHA256.new
        when 384 then Digest::SHA384.new
        when 512 then Digest::SHA512.new
        else raise ArgumentError, "unsupported bit length: #{bitlen.inspect}"
        end
      @bitlen = bitlen
    end

    def reset
      @sha2.reset
      self
    end

    def update(string)
      @sha2.update(string)
      self
    end
    alias << update

    private def finish
      @sha2.digest
    end

    def block_length = @sha2.block_length
    def digest_length = @sha2.digest_length

    def initialize_copy(other)
      super
      @sha2 = @sha2.clone
    end

    def inspect = "#<#{self.class.name}:#{@bitlen} #{hexdigest}>"
  end
end
