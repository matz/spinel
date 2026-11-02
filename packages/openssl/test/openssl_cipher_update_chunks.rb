require "openssl"

def cipher(bits, decrypting, tag)
  c = OpenSSL::Cipher.new("aes-#{bits}-gcm")
  if decrypting
    c.decrypt
  else
    c.encrypt
  end
  c.key = "k" * (bits / 8)
  c.iv = "i" * 12
  c.auth_data = "context"
  c.auth_tag = tag if decrypting && !tag.empty?
  c
end

def show(label, bytes)
  puts "#{label}=#{bytes.unpack1('H*')}"
end

[128, 256].each do |bits|
  text = '0123456789abcdef' * 4 + "\x00\xfflast"
  e = cipher(bits, false, '')
  output = +''
  offset = 0
  [1, 15, 16, 17, 21].each do |size|
    part = e.update(text.byteslice(offset, size))
    show("aes#{bits}.boundary.#{offset}", part)
    output << part
    offset += size
    show("aes#{bits}.boundary.empty.#{offset}", e.update(''))
  end
  show("aes#{bits}.boundary.final", e.final)
  tag = e.auth_tag
  show("aes#{bits}.boundary.tag", tag)
  d = cipher(bits, true, tag)
  offset = 0
  [17, 1, 15, 16, 21].each do |size|
    show("aes#{bits}.decrypt.boundary.#{offset}", d.update(output.byteslice(offset, size)))
    offset += size
  end
  show("aes#{bits}.decrypt.boundary.final", d.final)

  e = cipher(bits, false, '')
  input = +'sample'
  show("aes#{bits}.mutable.update", e.update(input))
  input.replace('changed')
  show("aes#{bits}.mutable.final", e.final)
  show("aes#{bits}.mutable.tag", e.auth_tag)
  d = cipher(bits, true, '')
  show("aes#{bits}.late_tag.update", d.update(output))
  d.auth_tag = tag
  show("aes#{bits}.late_tag.final", d.final)
end

class CustomCipher < OpenSSL::Cipher
  def update(data, buffer = nil)
    'custom'
  end
end
puts "override=#{CustomCipher.new('aes-128-gcm').update('sample')}"
