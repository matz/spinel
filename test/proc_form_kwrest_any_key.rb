# A method taking a block has a proc-form clone that a dispatch calls; its
# `**kwrest` must take a String key a call brings, as the original's does.
class Mapper
  def get(*path_or_actions, as: nil, to: nil, **mapping, &block)
    block&.call
    [path_or_actions, as, to, mapping]
  end
end
class Session
  def get(uri, params = {}, &block) = [:session, uri, params]
end
class RouteSet
  def initialize = @blocks = []
  def draw(&block) = @blocks << block
  def run(mapper) = @blocks.each { |b| b.call(mapper) }
end
set = RouteSet.new
set.draw do |m|
  p m.get("/hello" => "hello#index")
  p m.get("/post", to: "hello#create", as: :post) { :blk }
end
set.run(Mapper.new)
set.run(Session.new)
p Mapper.new.get("/direct" => "x")
