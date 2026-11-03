require 'openssl'

def configured(bits, iv, decrypting, tag)
  c = OpenSSL::Cipher.new("aes-#{bits}-gcm")
  decrypting ? c.decrypt : c.encrypt
  c.key = 'k' * (bits / 8)
  c.iv_len = iv.bytesize
  c.iv = iv
  c.auth_data = 'context'
  c.auth_tag = tag if decrypting
  c
end

[128, 256].each do |bits|
  [1, 8, 12, 16, 32].each do |iv_size|
    iv = 'i' * iv_size
    ['binary', 'utf8', 'empty'].each do |kind|
      text = if kind == 'binary'
        (0...128).map { |i| (i * 37) % 256 }.pack('C*')
      elsif kind == 'utf8'
        "\u00e9\u3042\u{1f642}" * 9
      else
        ''
      end
      label = "aes#{bits}.iv#{iv_size}.#{kind}"
      c = configured(bits, iv, false, '')
      encrypted = +''.b
      offset = 0
      [0, 1, 15, 0, 16, 17, 33, 46].each do |size|
        part = text.byteslice(offset, size) || ''.b
        result = c.update(part)
        puts "#{label}.#{offset}.#{size}=#{result.unpack1('H*')}"
        puts "#{label}.encoding=#{result.encoding.name}"
        encrypted << result
        offset += part.bytesize
      end
      encrypted << c.final
      tag = c.auth_tag
      puts "#{label}.tag=#{tag.unpack1('H*')}"
      d = configured(bits, iv, true, tag)
      plain = +''.b
      offset = 0
      [1, 15, 16, 17, 33, 46, 0].each do |size|
        part = encrypted.byteslice(offset, size) || ''.b
        plain << d.update(part)
        offset += part.bytesize
      end
      plain << d.final
      puts "#{label}.plaintext=#{plain.unpack1('H*')}"
    end
  end
end

# Changing a returned String must never mutate the private keystream.
c = configured(128, 'i' * 12, false, '')
result = c.update('s')
result.replace('x' * 1024)
empty = c.update('')
empty.replace('x' * 1024)
puts "private.next=#{c.update('ample').unpack1('H*')}"
puts "private.final=#{c.final.unpack1('H*')}"
puts "private.tag=#{c.auth_tag.unpack1('H*')}"
