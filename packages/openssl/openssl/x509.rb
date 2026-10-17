# OpenSSL::X509::Certificate -- a certificate is its DER, the way a key is its
# bytes (#9341). The spellings are CRuby's: new and load read PEM or DER, and
# to_der and to_pem write it back out.
#
# That is all of it. A TLS server hands its certificate to OpenSSL whole, and
# these four are what that takes. The fields a certificate carries (subject,
# issuer, not_after, extensions) are not here; reading one is CRuby's
# NoMethodError, as for the rest of X509 this package does not define.
#
# The PEM is parsed once, in C, where it crosses the boundary, and nothing on
# the C side outlives the call: there is no X509 * to free and no handle to
# run out of. An abandoned certificate costs its bytes.
module OpenSSL
  module X509
    class CertificateError < OpenSSLError
    end

    class Certificate
      # Every certificate in a PEM string, in order: for a fullchain.pem, the
      # leaf and then the certificates that issued it. A DER string holds one
      # certificate, and answers an Array of that one, as in CRuby.
      def self.load(data)
        blob = Native.x509_load(data)
        raise CertificateError, Native.last_error if blob.empty?
        certs = []
        at = 0
        while at < blob.bytesize
          len = blob.byteslice(at, 4).unpack1("N")
          certs << new(blob.byteslice(at + 4, len))
          at += 4 + len
        end
        certs
      end

      # PEM or DER, as in CRuby. The DER is what is kept.
      def initialize(data)
        @der = Native.x509_der(data)
        raise CertificateError, Native.last_error if @der.empty?
      end

      # A copy, as CRuby answers a new String each call: a caller that changes
      # what it was given does not change the certificate.
      def to_der
        @der.dup
      end

      def to_pem
        Native.x509_pem(@der)
      end
    end
  end
end
