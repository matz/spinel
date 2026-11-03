# An empty hash frozen where it is written (`@h = {}.freeze`) takes the
# keys a copy of it is given, as `@h = {}` does.

class Registry
  def initialize = @handlers = {}.freeze

  def register(name, handler)
    handlers = @handlers.dup
    handlers[name.to_sym] = handler
    @handlers = handlers.freeze
  end

  def handlers = @handlers
  def first_copy = (@handlers.dup[:raw] = 3)
end

r = Registry.new
r.register("erb", 1)
r.register(:builder, 2)
p r.handlers
p r.handlers.frozen?
p r.first_copy

module Handlers
  @template_handlers = {}.freeze
  @template_extensions = [].freeze

  class << self
    def extensions = @template_extensions
    def handlers = @template_handlers

    def register(*extensions, handler)
      handlers = @template_handlers.dup
      extensions.each do |extension|
        handlers[extension.to_sym] = handler
      end
      @template_handlers = handlers.freeze
      @template_extensions = @template_handlers.keys.freeze
    end
  end
end

p Handlers.handlers.frozen?
Handlers.register(:erb, :builder, 1)
Handlers.register("raw", 2)
p Handlers.handlers
p Handlers.extensions
p Handlers.handlers.frozen?

module Table
  @rows = {}.freeze
  def self.rows = @rows
end

p Table.rows.frozen?
begin
  Table.rows[:a] = 1
rescue FrozenError => e
  p e.class
end
p Table.rows
