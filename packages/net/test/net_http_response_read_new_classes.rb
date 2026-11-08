# Every named status and every fallback family uses CRuby's response class.
require "net/http"

CODES = %w[
  100 101 102 103
  200 201 202 203 204 205 206 207 208 226
  300 301 302 303 304 305 307 308
  400 401 402 403 404 405 406 407 408 409 410 411 412 413 414 415 416 417
  421 422 423 424 426 428 429 431 451
  500 501 502 503 504 505 506 507 508 510 511
  199 299 399 418 499 599 000 999
]

CODES.each do |code|
  reader, writer = IO.pipe
  writer.write("HTTP/1.1 #{code} Example\r\n\r\n")
  writer.close
  response = Net::HTTPResponse.read_new(Net::BufferedIO.new(reader))
  puts response.class
  p [response.code == code, response.http_version == "1.1", response.message == "Example"]
  reader.close
end

["HTTP 200\r\n\r\n", "HTTP/1.1 200\r\n\r\n"].each do |data|
  reader, writer = IO.pipe
  writer.write(data)
  writer.close
  response = ::Net::HTTPResponse.read_new(Net::BufferedIO.new(reader))
  p [response.http_version.nil?, response.message.nil?]
  reader.close
end
