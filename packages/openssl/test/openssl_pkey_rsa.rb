# PKey.read over an RSA key: a PKCS#8 and a PKCS#1 private key and a public
# key in both of its forms, as PEM and as DER; then CRuby's sign and sign_pss
# over the private half, checked by verify and verify_pss over the public
# one.
require "openssl"

# A 2048-bit key made for these tests, in the PKCS#8 form `openssl genpkey`
# writes and the PKCS#1 form `openssl rsa -traditional` writes, and its public
# half as a PUBLIC KEY and as a PKCS#1 RSA PUBLIC KEY.
PKCS8_PEM = <<~PEM
-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCuKHGt1duTXstD
NEBfMwIZY6Ee333kgKrahcOHqpGOIIOxxarIBBInD1YZR+38R9oNu0tbiUtlpvOp
0CO8ozKktsIieb667zrcGVyI//KTgtx8xaHamcu/jfwjbWNYbxtz7Vx2PM4NO3OE
9cAD1cj9Jx20biy6VQyJcBYLjkNdIJYXWJx5Ij2oatwvFJN06WrXzhGeefw57yfW
ivju3qBatdy8YB0nvZ17QBq7yEorWXCOH7xcOCoLAFu8oKGVP2//dAWjuzRn6PAh
jJ59RoOpo1j6bjKYJtUDPw+heq7QF175Xv7sKczLo8Syn2xrGl6LC3/TyHJ6gzhX
QzWo9GH9AgMBAAECggEAF/5gcwekobAMpuya9cKsRl0whHVb/dAwObz2DEnWldMq
8aH+/mF5MsJtJTyAsCIiC4RDRJAswAt5RGr1kXaWcpTvOBq3yT9OR6aC1ikU/Xeo
8tOzkc9gf7qLA0J9zmA4UwdflraX2MDZEHdfBeg9MYtjD0miheVTvdegYeUuqLSr
Qq7oso4vr6dsQsg8+mVWfeofnbDie8AUjiY7NeGAEp7jA61fDgVulcbX/hi42q7V
qODfFpjs6fBxGa6zbDMhWz6dqDa7PTH5ne4A6TqWfdg0cvZfkD6H/0uop3csmIfH
TfFh+EFyDVvMli030TTNwe0JT/SwzKaBhbbRbpqWDQKBgQDjMTlFdj8iRN73G1yI
mfHYJBJ9pcCaWoVN8P8zGebefpaJjWNo+NToTxO+D9wUjp5tI3l9DeUwhtUAJJIs
V9pEq8MzIM4ROmcXGfbJNlKd00reYtUCJrSCzyt8Ivj5tEaf1sHfIn+fyRRiHeC9
0DPLwdZfaO3Ox65UlLeRrjEkpwKBgQDEPbNGhmRcokKpRkljGbtaspTGpNAsWjka
gb/KXX8Atxgpdc1cnicYfMwcHQC9Cx4uxs9qusY3KOQFMtdzM4M8cQl083of3M7q
skDVx+qw48sopaNhpYdLqKVP+2DmtOITNcLO0lQ6mMAnaKlN0u/nDxqNk7X1HnOF
Mjf3aSQEuwKBgFf8h0acfeTisUsYAKUyoZPonHSmzcm5hujhjaQXwu6mLdzFshtE
SwhRH7862yvm8/omg/VnfTxm63TrtOSRfYmcz7MsYEIR0T0EJCxW6nYPD27C7eWw
YbZLT3b7AFuMctGSVXD6Jl0Wfdtl4NUCHh0qS8CVnYu4j45YLiOwR33/AoGAOAAb
fs6vKTgsqb2jB6K6MmGvzxgNR/rfPM81+fvAWGXngT1aw0lmgrhqTsOACHJ5xIN7
gUuXcPo8R7eAgCF9G+xt9G/es+UBLzx7AAay1JPMz+akvyLLfy27Lx3fQzbvJOg8
lWS0IJyiiRx24/yD9X3759b1H+P2t1XjXrkkTC0CgYBBtnkV+zDGR4SoublalEPx
irE3XvK2IkyDMKFUaCGshVgZ0h4co7wKL3Nyh1i/8PHn914ak77Gh9gMgcLitNgz
kKJEgovRpvFnTvc6ivVxdkMjA0a0C+6xlCDuPML7yy32hONbXgMrUdFcm/49lgIy
M3S+PteQRbYKXCT1Vl48eQ==
-----END PRIVATE KEY-----
PEM
PKCS1_PEM = <<~PEM
-----BEGIN RSA PRIVATE KEY-----
MIIEogIBAAKCAQEArihxrdXbk17LQzRAXzMCGWOhHt995ICq2oXDh6qRjiCDscWq
yAQSJw9WGUft/EfaDbtLW4lLZabzqdAjvKMypLbCInm+uu863BlciP/yk4LcfMWh
2pnLv438I21jWG8bc+1cdjzODTtzhPXAA9XI/ScdtG4sulUMiXAWC45DXSCWF1ic
eSI9qGrcLxSTdOlq184Rnnn8Oe8n1or47t6gWrXcvGAdJ72de0Aau8hKK1lwjh+8
XDgqCwBbvKChlT9v/3QFo7s0Z+jwIYyefUaDqaNY+m4ymCbVAz8PoXqu0Bde+V7+
7CnMy6PEsp9saxpeiwt/08hyeoM4V0M1qPRh/QIDAQABAoIBABf+YHMHpKGwDKbs
mvXCrEZdMIR1W/3QMDm89gxJ1pXTKvGh/v5heTLCbSU8gLAiIguEQ0SQLMALeURq
9ZF2lnKU7zgat8k/TkemgtYpFP13qPLTs5HPYH+6iwNCfc5gOFMHX5a2l9jA2RB3
XwXoPTGLYw9JooXlU73XoGHlLqi0q0Ku6LKOL6+nbELIPPplVn3qH52w4nvAFI4m
OzXhgBKe4wOtXw4FbpXG1/4YuNqu1ajg3xaY7OnwcRmus2wzIVs+nag2uz0x+Z3u
AOk6ln3YNHL2X5A+h/9LqKd3LJiHx03xYfhBcg1bzJYtN9E0zcHtCU/0sMymgYW2
0W6alg0CgYEA4zE5RXY/IkTe9xtciJnx2CQSfaXAmlqFTfD/Mxnm3n6WiY1jaPjU
6E8Tvg/cFI6ebSN5fQ3lMIbVACSSLFfaRKvDMyDOETpnFxn2yTZSndNK3mLVAia0
gs8rfCL4+bRGn9bB3yJ/n8kUYh3gvdAzy8HWX2jtzseuVJS3ka4xJKcCgYEAxD2z
RoZkXKJCqUZJYxm7WrKUxqTQLFo5GoG/yl1/ALcYKXXNXJ4nGHzMHB0AvQseLsbP
arrGNyjkBTLXczODPHEJdPN6H9zO6rJA1cfqsOPLKKWjYaWHS6ilT/tg5rTiEzXC
ztJUOpjAJ2ipTdLv5w8ajZO19R5zhTI392kkBLsCgYBX/IdGnH3k4rFLGAClMqGT
6Jx0ps3JuYbo4Y2kF8Lupi3cxbIbREsIUR+/Otsr5vP6JoP1Z308Zut067TkkX2J
nM+zLGBCEdE9BCQsVup2Dw9uwu3lsGG2S092+wBbjHLRklVw+iZdFn3bZeDVAh4d
KkvAlZ2LuI+OWC4jsEd9/wKBgDgAG37Oryk4LKm9oweiujJhr88YDUf63zzPNfn7
wFhl54E9WsNJZoK4ak7DgAhyecSDe4FLl3D6PEe3gIAhfRvsbfRv3rPlAS88ewAG
stSTzM/mpL8iy38tuy8d30M27yToPJVktCCcookcduP8g/V9++fW9R/j9rdV4165
JEwtAoGAQbZ5FfswxkeEqLm5WpRD8YqxN17ytiJMgzChVGghrIVYGdIeHKO8Ci9z
codYv/Dx5/deGpO+xofYDIHC4rTYM5CiRIKL0abxZ073Oor1cXZDIwNGtAvusZQg
7jzC+8st9oTjW14DK1HRXJv+PZYCMjN0vj7XkEW2Clwk9VZePHk=
-----END RSA PRIVATE KEY-----
PEM
PUBLIC_PEM = <<~PEM
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArihxrdXbk17LQzRAXzMC
GWOhHt995ICq2oXDh6qRjiCDscWqyAQSJw9WGUft/EfaDbtLW4lLZabzqdAjvKMy
pLbCInm+uu863BlciP/yk4LcfMWh2pnLv438I21jWG8bc+1cdjzODTtzhPXAA9XI
/ScdtG4sulUMiXAWC45DXSCWF1iceSI9qGrcLxSTdOlq184Rnnn8Oe8n1or47t6g
WrXcvGAdJ72de0Aau8hKK1lwjh+8XDgqCwBbvKChlT9v/3QFo7s0Z+jwIYyefUaD
qaNY+m4ymCbVAz8PoXqu0Bde+V7+7CnMy6PEsp9saxpeiwt/08hyeoM4V0M1qPRh
/QIDAQAB
-----END PUBLIC KEY-----
PEM
PKCS1_PUBLIC_PEM = <<~PEM
-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEArihxrdXbk17LQzRAXzMCGWOhHt995ICq2oXDh6qRjiCDscWqyAQS
Jw9WGUft/EfaDbtLW4lLZabzqdAjvKMypLbCInm+uu863BlciP/yk4LcfMWh2pnL
v438I21jWG8bc+1cdjzODTtzhPXAA9XI/ScdtG4sulUMiXAWC45DXSCWF1iceSI9
qGrcLxSTdOlq184Rnnn8Oe8n1or47t6gWrXcvGAdJ72de0Aau8hKK1lwjh+8XDgq
CwBbvKChlT9v/3QFo7s0Z+jwIYyefUaDqaNY+m4ymCbVAz8PoXqu0Bde+V7+7CnM
y6PEsp9saxpeiwt/08hyeoM4V0M1qPRh/QIDAQAB
-----END RSA PUBLIC KEY-----
PEM

def der(pem) = pem.lines.reject { |l| l.start_with?("-----") }.join.unpack1("m")

[PKCS8_PEM, PKCS1_PEM, PUBLIC_PEM, PKCS1_PUBLIC_PEM].each do |pem|
  k = OpenSSL::PKey.read(pem)
  p [k.class, k.private?, k.public?]
end
[PKCS8_PEM, PKCS1_PEM, PUBLIC_PEM].each do |pem|
  k = OpenSSL::PKey.read(der(pem))
  p [k.class, k.private?]
end

key = OpenSSL::PKey.read(PKCS1_PEM)
pub = OpenSSL::PKey.read(PUBLIC_PEM)
data = "Banpo Bridge to Jamsil"

# PKCS#1 v1.5 is deterministic, so the signature is the very bytes CRuby
# makes: its digest is printed.
sig = key.sign("SHA256", data)
p sig.bytesize
p OpenSSL::Digest::SHA256.hexdigest(sig)
p OpenSSL::PKey.read(PKCS8_PEM).sign(OpenSSL::Digest.new("SHA256"), data) == sig
p pub.verify("SHA256", sig, data)
p pub.verify("SHA256", sig, "Banpo Bridge to Gimpo")

# PSS is salted, so two signatures differ, and both verify.
pss = key.sign_pss("SHA256", data, salt_length: :digest, mgf1_hash: "SHA256")
again = key.sign_pss("SHA256", data, salt_length: :max, mgf1_hash: "SHA256")
p pss.bytesize, pss == again
p pub.verify_pss("SHA256", pss, data, salt_length: :auto, mgf1_hash: "SHA256")
p pub.verify_pss("SHA256", again, data, salt_length: :auto, mgf1_hash: "SHA256")
p pub.verify_pss("SHA256", pss, data, salt_length: 32, mgf1_hash: "SHA256")

# public_key is the public half alone.
half = key.public_key
p [half.class, half.private?, half.verify("SHA256", sig, data)]

begin
  pub.sign("SHA256", data)
rescue ArgumentError => e
  p e.message
end
begin
  key.sign("Seoul-512", data)
rescue OpenSSL::Digest::DigestError => e
  p e.class
end
begin
  key.sign_pss("SHA256", data, salt_length: :auto, mgf1_hash: "SHA256")
rescue TypeError => e
  p e.message
end

# Earlier EC failures must not contaminate an unrelated RSA failure. Compare
# its message with itself in this process, so no library-specific text is
# printed or recorded in the expected output.
def oversized_salt_error(key)
  key.sign_pss("SHA256", "data", salt_length: 10000, mgf1_hash: "SHA256")
  ""
rescue OpenSSL::PKey::PKeyError => error
  puts error.class
  error.message
end

expected_error = oversized_salt_error(key)
# Scalar 1 gives Q = G; r = s = 1 with digest n - 1 makes verification reach
# the point at infinity. This negative return queues an EC error even on
# libraries whose DER decoder does not queue errors for malformed input.
ec = OpenSSL::PKey.read(["30310201010420" + "0" * 63 + "1" + "a00a06082a8648ce3d030107"].pack("H*"))
digest = ["ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550"].pack("H*")
signature = ["3006020101020101"].pack("H*")
[signature, "broken", signature + "\0"].each do |invalid|
  begin
    ec.dsa_verify_asn1(digest, invalid)
  rescue OpenSSL::PKey::PKeyError => error
    puts error.class
  end
  p oversized_salt_error(key) == expected_error
end
begin
  OpenSSL::PKey::EC::Group.new("unknown-curve")
rescue OpenSSL::PKey::EC::Group::Error => error
  puts error.class
end
p oversized_salt_error(key) == expected_error
