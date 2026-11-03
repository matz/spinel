# The other spellings of a block parameter's call, and a class method's own
# class methods reached in other forms: each stores the block's value into a
# Hash of mixed values and answers it.

class Ctx
  def initialize(n) = @n = n
  def n = @n
end

module Paren
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
  def self.fetch(key, &block) = get(key) || set(key, block.())
end

module Index
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
  def self.fetch(key, &block) = get(key) || set(key, block[key])
end

module Singleton
  SLOTS = {}
  class << self
    def get(key) = SLOTS[key]
    def set(key, value) = SLOTS[key] = value
    def fetch(key) = get(key) || set(key, yield)
  end
end

module Fn
  SLOTS = {}
  module_function

  def get(key) = SLOTS[key]
  def set(key, value) = SLOTS[key] = value
  def fetch(key) = get(key) || set(key, yield)
end

class Base
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
end

class Sub < Base
  def self.fetch(key) = get(key) || set(key, yield)
end

module Nested
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
  def self.fetch(key) = get(key) || [key].map { |k| set(k, yield) }.first
end

p [Paren.fetch(:count) { 3 }, Paren.fetch(:name) { "kale" }, Paren.fetch(:ctx) { Ctx.new(2) }.n, Paren.fetch(:list) { [1, 2] }]
p [Index.fetch(:count) { 3 }, Index.fetch(:name) { "kale" }, Index.fetch(:ctx) { Ctx.new(2) }.n, Index.fetch(:list) { [1, 2] }]
p [Singleton.fetch(:count) { 3 }, Singleton.fetch(:name) { "kale" }, Singleton.fetch(:ctx) { Ctx.new(2) }.n, Singleton.fetch(:list) { [1, 2] }]
p [Fn.fetch(:count) { 3 }, Fn.fetch(:name) { "kale" }, Fn.fetch(:ctx) { Ctx.new(2) }.n, Fn.fetch(:list) { [1, 2] }]
p [Sub.fetch(:count) { 3 }, Sub.fetch(:name) { "kale" }, Sub.fetch(:ctx) { Ctx.new(2) }.n, Sub.fetch(:list) { [1, 2] }]
p [Nested.fetch(:count) { 3 }, Nested.fetch(:name) { "kale" }, Nested.fetch(:ctx) { Ctx.new(2) }.n, Nested.fetch(:list) { [1, 2] }]
