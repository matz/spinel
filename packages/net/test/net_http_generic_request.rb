# Net::HTTPGenericRequest is the base of Net::HTTPRequest and of
# Net::HTTP::Get and the rest, and code that forwards a request of any
# method builds one directly (#11347)
require "net/http"
req = Net::HTTPGenericRequest.new("POST", true, true, "/x")
p req.method
p req.path
p req.request_body_permitted?
p req.response_body_permitted?
p req["accept"]
g = Net::HTTPGenericRequest.new("GET", false, true, "/y", { "X-A" => "1" })
p g["x-a"]
p g.request_body_permitted?
p Net::HTTP::Get.new("/z").is_a?(Net::HTTPGenericRequest)
p Net::HTTP::Get.new("/z").is_a?(Net::HTTPRequest)
p Net::HTTP::Head.new("/h").response_body_permitted?
p Net::HTTP::Post.new("/p").request_body_permitted?
p Net::HTTP::Delete.new("/d").request_body_permitted?
