require "openssl"

def decrypt(key, nonce, ciphertext, tag, aad = "")
  cipher = OpenSSL::Cipher.new("aes-128-gcm").decrypt
  cipher.key = key
  cipher.iv = nonce
  cipher.auth_tag = tag if tag
  cipher.auth_data = aad
  output = ciphertext.empty? ? "" : cipher.update(ciphertext)
  output + cipher.final
end

# NIST's all-zero AES-128-GCM vectors, with full and empty plaintext.
key = ("\0" * 16).freeze
nonce = ("\0" * 12).freeze
ciphertext = ["0388dace60b6a392f328c2b971b2fe78"].pack("H*").freeze
tag = ["ab6e47d42cec13bdf53a67b21257bddf"].pack("H*").freeze
empty_tag = ["58e2fccefa7e3061367f1d57a4e7455a"].pack("H*").freeze
p decrypt(key, nonce, ciphertext, tag) == "\0" * 16
p decrypt(key, nonce, "", empty_tag) == ""

# Retrieving a tag before encryption finishes keeps the broader class.
begin
  OpenSSL::Cipher.new("aes-128-gcm").encrypt.auth_tag
rescue OpenSSL::Cipher::AuthTagError
  puts "wrong tag retrieval class"
rescue OpenSSL::Cipher::CipherError => error
  puts "tag retrieval: #{error.class}"
end

bad_tag = tag.dup
bad_tag.setbyte(0, tag.getbyte(0) ^ 1)
bad_ciphertext = ciphertext.dup
bad_ciphertext.setbyte(0, ciphertext.getbyte(0) ^ 1)
cases = [
  ["altered tag", key, nonce, ciphertext, bad_tag.freeze, ""],
  ["altered ciphertext", key, nonce, bad_ciphertext.freeze, tag, ""],
  ["wrong key", "\1" + key.byteslice(1, 15), nonce, ciphertext, tag, ""],
  ["wrong nonce", key, "\1" + nonce.byteslice(1, 11), ciphertext, tag, ""],
  ["missing tag", key, nonce, ciphertext, nil, ""],
  ["altered AAD", key, nonce, ciphertext, tag, "changed"],
  ["empty altered tag", key, nonce, "", "\0" * 16, ""]
]
cases.each do |label, case_key, case_nonce, data, case_tag, aad|
  begin
    decrypt(case_key, case_nonce, data, case_tag, aad)
  rescue OpenSSL::Cipher::AuthTagError => error
    puts "#{label}: #{error.class}: #{error.message.start_with?("AEAD authentication tag verification failed")}"
  end
  begin
    decrypt(case_key, case_nonce, data, case_tag, aad)
  rescue OpenSSL::Cipher::CipherError => error
    puts "#{label}: rescued by CipherError: #{error.class}"
  end
end
