# spinel: needs-rbs-parser
# An unfinished receiver must not widen an unrelated getter's field.
# infer-test checks the fields: program output alone also passes without the fix.
module Tep
  def self.str_hash = Hash.new("")
  def self.key(flag) = flag ? "hello" : 1

  class Response
    attr_reader :headers #: Hash[String, String]
    def initialize = @headers = Tep.str_hash
  end
  class Entry
    attr_reader :headers
    def initialize(headers) = @headers = headers
  end
  class Cache
    def initialize
      headers = Tep.str_hash
      headers["hello"] = "world"
      @items = {"one" => Entry.new(headers)}
    end
    def answer(key, res)
      Cache.fill(@items["one"], res)
    end
    def self.fill(entry, res)
      entry.headers.each { |k,v| res.headers[Tep.key(true)] = v }
    end
  end
  CACHE = Cache.new
  def self.hop0(key, res) = Tep::CACHE.answer(key, res)
  def self.run
    res = Response.new
    Tep.hop0("one", res)
    p res.headers
  end
end
Tep.run
