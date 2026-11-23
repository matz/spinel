# A rescue arm and Exception#is_a? walk a raised exception's class chain by
# name. The builtin hierarchy and the program's own are looked up through a
# hash, not a scan of every row: the answers are the same for a deep chain,
# for a class name shared by two namespaces (each answers its own parent),
# for many classes (whose names share slots), and for builtin classes that
# have no class id (JSON::ParserError, Errno::*).
require "json"

module Web
  class Error < StandardError; end
  class NotFound < Error; end
  class RoutingError < NotFound; end
end
module Db
  class Error < RuntimeError; end
  class NotFound < Error; end
end

def kinds(e)
  [Web::Error, Web::NotFound, Web::RoutingError, Db::Error, Db::NotFound,
   RuntimeError, StandardError, Exception].map { |k| e.is_a?(k) ? 1 : 0 }.join
end
p kinds(Web::RoutingError.new)
p kinds(Db::NotFound.new)
p kinds(Web::NotFound.new)

def arm(err)
  raise err
rescue Db::Error
  :db
rescue Web::NotFound
  :web_not_found
rescue Web::Error
  :web
rescue RuntimeError
  :runtime
rescue => e
  e.class.name.to_sym
end
p arm(Web::RoutingError.new("x"))
p arm(Db::NotFound.new("x"))
p arm(Web::Error.new("x"))
p arm(FrozenError.new("x"))
p arm(KeyError.new("x"))

# many classes: a chain twenty deep, each a subclass of the one before
class E00 < StandardError; end
class E01 < E00; end
class E02 < E01; end
class E03 < E02; end
class E04 < E03; end
class E05 < E04; end
class E06 < E05; end
class E07 < E06; end
class E08 < E07; end
class E09 < E08; end
class E10 < E09; end
class E11 < E10; end
class E12 < E11; end
class E13 < E12; end
class E14 < E13; end
class E15 < E14; end
class E16 < E15; end
class E17 < E16; end
class E18 < E17; end
class E19 < E18; end
begin
  raise E19, "deep"
rescue E00 => e
  p [e.class, e.is_a?(E10), e.is_a?(StandardError), e.is_a?(Web::Error)]
end
begin
  raise E07
rescue E08
  p :wrong
rescue E05 => e
  p [e.class, e.is_a?(E08), e.is_a?(E06)]
end

# builtin classes the id hierarchy does not cover answer through the names
begin
  JSON.parse("{")
rescue JSON::JSONError => e
  p [e.class, e.is_a?(StandardError), e.is_a?(JSON::ParserError), e.is_a?(IndexError)]
end
begin
  File.read("/nonexistent/file/here")
rescue SystemCallError => e
  p [e.class, e.is_a?(StandardError), e.is_a?(Errno::ENOENT), e.is_a?(IOError)]
end
