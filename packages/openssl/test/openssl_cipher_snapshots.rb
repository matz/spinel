require 'openssl'

def decryptor(key, iv, aad, tag)
  c = OpenSSL::Cipher.new('aes-128-gcm')
  c.decrypt
  c.key = key
  c.iv = iv
  c.auth_data = aad
  c.auth_tag = tag
  c
end

ct = ['6e854b7b70d8'].pack('H*')
tag_hex = '8224eadf76fc5bd460c5ee719ac1cd3a'
['key', 'iv', 'aad', 'tag'].each do |field|
  key = 'k' * 16
  iv = 'i' * 12
  aad = +'context'
  tag = [tag_hex].pack('H*')
  c = decryptor(key, iv, aad, tag)
  case field
  when 'key' then key.replace('x' * 16)
  when 'iv' then iv.replace('x' * 12)
  when 'aad' then aad.replace('changed')
  when 'tag' then tag.replace('x' * 16)
  end
  begin
    out = c.update(ct) + c.final
    puts "#{field}.plaintext=#{out.unpack1('H*')}"
  rescue OpenSSL::OpenSSLError
    puts "#{field}.plaintext=rejected"
  end
end

['key', 'iv'].each do |field|
  key = 'k' * 16
  iv = 'i' * 12
  c = decryptor(key, iv, 'context', [tag_hex].pack('H*'))
  out = c.update(ct.byteslice(0, 3))
  if field == 'key'
    key.replace('x' * 16)
  else
    iv.replace('x' * 12)
  end
  out += c.update(ct.byteslice(3, 3))
  key.replace('k' * 16)
  iv.replace('i' * 12)
  out += c.final
  puts "transient.#{field}=#{out.unpack1('H*')}"
end

c = OpenSSL::Cipher.new('aes-128-gcm')
c.encrypt
key = 'k' * 16
iv = 'i' * 12
aad = +'context'
puts "key.return=#{c.send(:key=, key).equal?(key)}"
puts "iv.return=#{c.send(:iv=, iv).equal?(iv)}"
puts "aad.return=#{c.send(:auth_data=, aad).equal?(aad)}"
name = c.name
saved_name = String.new(name)
name.replace('changed')
puts "name.snapshot=#{c.name == saved_name}"
c = OpenSSL::Cipher.new('aes-128-gcm')
c.encrypt
c.key = key
c.iv = iv
c.auth_data = aad
c.update('sample')
c.final
tag = c.auth_tag
saved_tag = String.new(tag)
tag.replace('x' * 16)
puts "tag.snapshot=#{c.auth_tag == saved_tag}"

bad_tag = 'x' * 16
c = decryptor(key, iv, aad, bad_tag)
bad_tag.replace([tag_hex].pack('H*'))
begin
  c.update(ct)
  c.final
  puts 'changed_bad_tag=ACCEPTED'
rescue OpenSSL::OpenSSLError
  puts 'changed_bad_tag=rejected'
end

['key', 'iv'].each do |field|
  c = OpenSSL::Cipher.new('aes-128-gcm')
  c.encrypt
  key = c.random_key
  iv = c.random_iv
  saved_key = String.new(key)
  saved_iv = String.new(iv)
  if field == 'key'
    key.replace('x' * 16)
  else
    iv.replace('x' * 12)
  end
  actual = c.update('sample') + c.final
  ref = OpenSSL::Cipher.new('aes-128-gcm')
  ref.encrypt
  ref.key = saved_key
  ref.iv = saved_iv
  expected = ref.update('sample') + ref.final
  puts "random.#{field}.snapshot=#{actual == expected && c.auth_tag == ref.auth_tag}"
end
