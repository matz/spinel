# A request asks for gzip and deflate and decodes the answer, unless the
# caller passes Accept-Encoding or Range (even nil) or sets Accept-Encoding
# later. HEAD asks too, but has no body to decode.
require "net/http"

requests = [
  Net::HTTP::Get.new("/"),
  Net::HTTP::Post.new("/"),
  Net::HTTP::Head.new("/"),
  Net::HTTP::Get.new("/", "AcCePt-EnCoDiNg" => "gzip"),
  Net::HTTP::Get.new("/", "Range" => "bytes=0-10"),
  Net::HTTP::Get.new("/", "accept-encoding" => nil),
  Net::HTTP::Get.new("/", "range" => nil),
]
requests.each { |r| p [r["accept-encoding"], r.decode_content] }

r = Net::HTTP::Get.new("/")
r["ACCEPT-ENCODING"] = "gzip"
p [r["accept-encoding"], r.decode_content]
r = Net::HTTP::Get.new("/")
r["Accept-Encoding"] = nil
p [r["accept-encoding"], r.decode_content]
r = Net::HTTP::Get.new("/")
r["Range"] = "bytes=0-10"
p [r["accept-encoding"], r.decode_content]

headers = { "X-Client" => "mine" }
r = Net::HTTP::Get.new("/", headers)
p headers
p [r["accept-encoding"], r.decode_content]
