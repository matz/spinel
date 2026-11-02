require 'openssl'

class CipherInput < String
  def dup
    'changed'
  end

  def to_s
    'changed'
  end

  def to_str
    'changed'
  end

  def initialize_copy(other)
    raise 'must not call initialize_copy'
  end
end

c = OpenSSL::Cipher.new('aes-128-gcm')
c.encrypt
c.key = CipherInput.new('k' * 16)
c.iv = CipherInput.new('i' * 12)
c.auth_data = CipherInput.new('context')
out = c.update(CipherInput.new('sample')) + c.final
puts "ciphertext=#{out.unpack1('H*')}"
puts "tag=#{c.auth_tag.unpack1('H*')}"
d = OpenSSL::Cipher.new('aes-128-gcm')
d.decrypt
d.key = CipherInput.new('k' * 16)
d.iv = CipherInput.new('i' * 12)
d.auth_data = CipherInput.new('context')
tag = ['8224eadf76fc5bd460c5ee719ac1cd3a'].pack('H*')
d.auth_tag = CipherInput.new(tag)
puts "plaintext=#{(d.update(['6e854b7b70d8'].pack('H*')) + d.final).unpack1('H*')}"
