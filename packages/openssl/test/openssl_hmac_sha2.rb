require "openssl"

# RFC 4231, case 1. The key and message are public test-vector data.
key = ("\x0b" * 20).freeze
message = "Hi There".freeze
vectors = {
  "SHA384" => "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfa" +
              "ea9ea9076ede7f4af152e8b2fa9cb6",
  "SHA512" => "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde" +
              "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854",
  "SHA256" => "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
}
vectors.each do |name, expected|
  digest = OpenSSL::Digest.new(name.downcase, "ignored")
  p OpenSSL::HMAC.hexdigest(name.downcase, key, message) == expected
  p OpenSSL::HMAC.hexdigest(digest, key, message) == expected
  p OpenSSL::HMAC.digest(digest, key, message).unpack1("H*") == expected
  hmac = OpenSSL::HMAC.new(key, digest)
  p hmac.update("Hi ").update("There").hexdigest == expected
  p hmac.dup.reset.update(message).digest.unpack1("H*") == expected
  p OpenSSL::HMAC.hexdigest(name, "wrong", message) == expected
end

# RFC 4231, case 6: a key longer than SHA512's 128-byte block.
puts OpenSSL::HMAC.hexdigest("SHA512", "\xaa" * 131,
                            "Test Using Larger Than Block-Size Key - Hash Key First")
puts OpenSSL::HMAC.hexdigest("SHA384", "\xaa" * 131,
                            "Test Using Larger Than Block-Size Key - Hash Key First")
puts OpenSSL::HMAC.hexdigest("SHA512", "", "")
puts OpenSSL::HMAC.hexdigest("SHA384", "\0\xff".b, "a\0\xff".b)

def show_error
  yield
rescue => e
  message = e.message
  message = message.downcase.split(/[(:]/).first.strip if e.is_a?(OpenSSL::Digest::DigestError)
  puts "#{e.class}: #{message}"
end
show_error { OpenSSL::HMAC.digest("nonsense", "key", "data") }
show_error { OpenSSL::HMAC.new("key", "nonsense") }
show_error { OpenSSL::HMAC.digest("SHA512", nil, "data") }
show_error { OpenSSL::HMAC.digest("SHA512", "key", nil) }
show_error { OpenSSL::HMAC.digest("SHA512\0bad", "key", "data") }

threads = 4.times.map do
  Thread.new do
    10.times do
      raise "SHA512" unless OpenSSL::HMAC.hexdigest("SHA512", key, message) == vectors["SHA512"]
      raise "SHA384" unless OpenSSL::HMAC.hexdigest("SHA384", key, message) == vectors["SHA384"]
    end
    true
  end
end
p threads.all? { |thread| thread.value }
