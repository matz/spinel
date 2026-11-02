# spinel: not-cruby -- it rescues the package's OpenSSL::CipherError, which CRuby spells OpenSSL::Cipher::CipherError
# An authentication verdict must survive other threads.
#
# The C answers 0x01 || plaintext for a message that authenticates and an empty
# string for one that does not, so the verdict is part of the answer. It used
# to be read from a separate last_error call, which is a different thing: an
# empty string is a legitimate plaintext (the message of zero bytes), so
# "empty" could not also mean "forged", and between the decrypt and the
# question another worker's success could clear the slot. A forged message then
# arrived as valid empty plaintext -- measured at 13 to 31 acceptances in 2400
# on this machine, so a run of this test on the old code fails rather than
# merely being unlucky.
require "openssl"

def hx(s) = [s].pack("H*")

KEY = hx("a088555b4e0c45dcb65cdf4288a2f14e")
IV  = hx("e21ffde6495727913faa7a0d")

e = OpenSSL::Cipher.new("aes-128-gcm")
e.encrypt
e.key = KEY
e.iv = IV
CT  = e.update("the quick brown fox") + e.final
TAG = e.auth_tag
BAD = hx("00") + TAG.byteslice(1, 15)

def decrypt(ct, tag)
  d = OpenSSL::Cipher.new("aes-128-gcm")
  d.decrypt
  d.key = KEY
  d.iv = IV
  d.auth_tag = tag
  d.update(ct) + d.final
end

# Half the workers forge and half send genuine messages, so each is running
# while the other is setting or clearing a failure.
threads = []
8.times do |t|
  threads << Thread.new do
    wrong = 0
    300.times do
      if t.even?
        # A genuine message must not be rejected because a neighbour failed.
        begin
          wrong += 1 unless decrypt(CT, TAG) == "the quick brown fox"
        rescue OpenSSL::CipherError
          wrong += 1
        end
      else
        # A forged one must not be accepted because a neighbour succeeded.
        begin
          decrypt(CT, BAD)
          wrong += 1
        rescue OpenSSL::CipherError
        end
      end
    end
    wrong
  end
end
p threads.map(&:value).sum

# The empty message is the case the old protocol could not express: its
# plaintext is empty and so is a forgery's answer.
z = OpenSSL::Cipher.new("aes-128-gcm")
z.encrypt
z.key = KEY
z.iv = IV
z.final
ztag = z.auth_tag

p decrypt("", ztag) == ""
begin
  decrypt("", hx("ff") + ztag.byteslice(1, 15))
  p "accepted a forged empty message"
rescue OpenSSL::CipherError => ex
  puts "CipherError: #{ex.message}"
end
