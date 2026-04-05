#include <cryptlib.h>
#include <rsa.h>
#include <osrng.h>
#include <files.h>
#include <integer.h>

#include <iostream>
#include <string>

using namespace CryptoPP;

int main() {
    AutoSeededRandomPool rng;

    std::cout << "[*] Generating RSA-2048 key pair...\n";

    InvertibleRSAFunction params;
    params.GenerateRandomWithKeySize(rng, 2048);

    RSA::PrivateKey privateKey(params);
    RSA::PublicKey publicKey(params);

    // Simpan PRIVATE KEY (PKCS#8 DER)
    {
        FileSink file("rsa_private.der");
        privateKey.Save(file);
        file.MessageEnd();
        std::cout << "[+] Private key saved: rsa_private.der\n";
    }

    // Simpan PUBLIC KEY (SubjectPublicKeyInfo DER)
    {
        FileSink file("rsa_public.der");
        publicKey.Save(file);
        file.MessageEnd();
        std::cout << "[+] Public key saved: rsa_public.der\n";
    }

    std::cout << "[*] Key pair generation selesai.\n";
    std::cout << "    Gunakan rsa_public.der untuk enkripsi,\n";
    std::cout << "    rsa_private.der untuk dekripsi (jaga kerahasiaan!).\n";

    return 0;
}
