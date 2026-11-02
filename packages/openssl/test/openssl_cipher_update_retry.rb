require 'openssl'
c = OpenSSL::Cipher.new('aes-128-gcm')
c.encrypt
c.iv = 'i' * 12
begin
  c.update('discard')
  puts 'first=accepted'
rescue OpenSSL::OpenSSLError
  puts 'first=rejected'
end
c.key = 'k' * 16
out = c.update('sample') + c.final
puts "ciphertext=#{out.unpack1('H*')}"
puts "tag=#{c.auth_tag.unpack1('H*')}"
