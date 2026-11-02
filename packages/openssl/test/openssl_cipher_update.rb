require "openssl"

def show(label, bytes)
  puts "#{label}=#{bytes.unpack1('H*')}"
end

def cipher(bits, decrypting, tag, aad, key_byte = "k")
  c = OpenSSL::Cipher.new("aes-#{bits}-gcm")
  if decrypting
    c.decrypt
  else
    c.encrypt
  end
  c.key = key_byte * (bits / 8)
  c.iv = "i" * 12
  c.auth_data = aad
  c.auth_tag = tag if decrypting
  c
end

[128, 256].each do |bits|
  label = "aes#{bits}"
  c = cipher(bits, false, "", "context")
  one = c.update("sample"); show("#{label}.encrypt.update", one)
  rest = c.final; show("#{label}.encrypt.final", rest)
  ct = one + rest; show("#{label}.encrypt.total", ct)
  tag = c.auth_tag; show("#{label}.encrypt.tag", tag)
  d = cipher(bits, true, tag, "context")
  show("#{label}.decrypt.update", d.update(ct))
  show("#{label}.decrypt.final", d.final)

  c = cipher(bits, false, "", "context")
  first = c.update("s"); show("#{label}.multi.update1", first)
  second = c.update("am"); show("#{label}.multi.update2", second)
  third = c.update("ple"); show("#{label}.multi.update3", third)
  last = c.final; show("#{label}.multi.final", last)
  show("#{label}.multi.total", first + second + third + last)
  show("#{label}.multi.tag", c.auth_tag)
  d = cipher(bits, true, tag, "context")
  show("#{label}.decrypt.multi1", d.update(ct.byteslice(0, 1)))
  show("#{label}.decrypt.multi2", d.update(ct.byteslice(1, 2)))
  show("#{label}.decrypt.multi3", d.update(ct.byteslice(3, 3)))
  show("#{label}.decrypt.multi_final", d.final)

  c = cipher(bits, false, "", "")
  begin
    show("#{label}.empty.update", c.update(""))
  rescue StandardError => ex
    puts "#{label}.empty.update=#{ex.class.name}:#{ex.message}"
  end
  show("#{label}.empty.final", c.final)
  show("#{label}.empty.tag", c.auth_tag)
  c = cipher(bits, false, "", "other context")
  show("#{label}.aad.update", c.update("sample"))
  show("#{label}.aad.final", c.final)
  show("#{label}.aad.tag", c.auth_tag)
  c = cipher(bits, false, "", "context")
  begin
    show("#{label}.early.tag", c.auth_tag)
  rescue StandardError
    puts "#{label}.early.tag=raises"
  end
  ["tag", "key", "aad", "ciphertext"].each do |kind|
    d = cipher(bits, true, kind == "tag" ? "x" * 16 : tag, kind == "aad" ? "changed" : "context", kind == "key" ? "x" : "k")
    input = kind == "ciphertext" ? "x" * 6 : ct
    show("#{label}.reject.#{kind}.update", d.update(input))
    begin
      show("#{label}.reject.#{kind}.final", d.final)
      puts "#{label}.reject.#{kind}=ACCEPTED"
    rescue OpenSSL::OpenSSLError
      puts "#{label}.reject.#{kind}=raises"
    end
  end
end
