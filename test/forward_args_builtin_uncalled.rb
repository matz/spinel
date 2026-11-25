# A `...` forwarder into a builtin that no call names, reached only
# through JSON's generator, takes the arguments the generator passes.
require "json"

class Request
  def as_json = {method: "GET", path: "/"}
  def to_json(...) = as_json.to_json(...)
end

class Response
  def as_json = {status: 200}
  def to_json(...) = as_json.to_json(...)
end

puts JSON.generate([Request.new, Response.new])
puts JSON.generate({r: Response.new})
