// Copyright (c) 2014 Blockstream
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include <secp256k1.h>

#include "stolen.h"

#define ERROREXIT(str...) do {fprintf(stderr, str); exit(1);} while(0)
#define USAGEEXIT(str...) do {fprintf(stderr, str); usage(); exit(1);} while (0)

void usage() {
	printf("USAGE: Generate address: -g -r <redeem script> (-d <Contract P2SH/regular address>)|(-a <ASCII Contract text>)  [-n <16-byte random nonce>]\n");
	printf("When generating the address, a random nonce is used unless one is specified\n");
	printf("If you do not care about privacy, anything may be used, otherwise some random value should be used\n");
	printf("Note that if the nonce is lost, your ability to redeem funds sent to the resulting address is also lost\n");
	printf("USAGE: Generate privkey: -c -p <base58 private key> (-d <Contract P2SH/regular address>)|(-a <ASCII Contract text>) -n <nonce>\n");
	printf("\n");
	printf("Example: contracthashtool -g -r 5121038695b28f1649c711aedb1fec8df54874334cfb7ddf31ba3132a94d00bdc9715251ae -d mqWkEAFeQdrQvyaWNRn5vijPJeiQAjtxL2\n");
	printf(" Where 5121038695b28f1649c711aedb1fec8df54874334cfb7ddf31ba3132a94d00bdc9715251ae is a hex-encoded Bitcoin script containing public keys in an obvious format (this one is 1-of-1 raw CHECKMULTISIG)\n");
	printf(" and mqWkEAFeQdrQvyaWNRn5vijPJeiQAjtxL2 is an address which is used to permute the public keys in the above script.\n");
	printf(" The holder of the private key in 512103... will then need the nonce, and mqWkEAFeQdrQvyaWNRn5vijPJeiQAjtxL2 to claim the funds.\n");
	printf(" The holder would then do something like contracthashtool -c -p cMcpaCT6pHkyS4347i4rSmecaQtLiu1eH28NWmBiePn8bi6N4kzh -d mqWkEAFeQdrQvyaWNRn5vijPJeiQAjtxL2 -n 3a11be476485a6273fad4a0e09117d42\n");
	printf(" They would then have the private key neccessary to claim the funds sent to the address -g... had generated\n");
	//TODO: Also, replace -d/-a/-n with -f for full contract specification (P2SH/P2PH only)
}

int get_pubkeys_from_redeemscript(unsigned char *redeem_script, unsigned int redeem_script_len, unsigned char* pubkeys[]) {
	unsigned char *readpos = redeem_script, * const endpos = redeem_script + redeem_script_len;
	unsigned char *maybe_keys[redeem_script_len/33];
	unsigned int maybe_keys_count = 0, pubkeys_count = 0;;
	bool require_next_checkmultisig = false;

	while (readpos < endpos) {
		int pushlen = -1;
		unsigned char* push_start = NULL;

		if (*readpos > 0 && *readpos < 76) {
			pushlen = *readpos;
			push_start = readpos + 1;
		} else if (*readpos == 76) {
			if (readpos + 1 >= endpos)
				ERROREXIT("Invalid push in script\n");
			pushlen = *(readpos + 1);
			push_start = readpos + 2;
		} else if (*readpos == 77) {
			if (readpos + 2 >= endpos)
				ERROREXIT("Invalid push in script\n");
			pushlen = *(readpos + 1) | (*(readpos + 2) << 8);
			push_start = readpos + 3;
		} else if (*readpos == 78) {
			if (readpos + 4 >= endpos)
				ERROREXIT("Invalid push in script\n");
			pushlen = *(readpos + 1) | (*(readpos + 2) << 8) | (*(readpos + 3) << 16) | (*(readpos + 4) << 24);
			push_start = readpos + 5;
		}

		if (pushlen > -1) {
			if (push_start + pushlen >= endpos)
				ERROREXIT("Invalid push in script\n");

			if (pushlen == 65 && *push_start == 4)
				ERROREXIT("ERROR: Possible uncompressed pubkey found in redeem script, not converting it\n");
			else if (pushlen == 33 && (*push_start == 2 || *push_start == 3))
				maybe_keys[maybe_keys_count++] = push_start;
			else if (maybe_keys_count > 0)
				ERROREXIT("ERROR: Found possible public keys but are not using them as they are not followed immediately by [OP_N] OP_CHECK[MULTI]SIG[VERIFY]\n");
		} else {
			if (require_next_checkmultisig) {
				if (*readpos == 174 || *readpos == 175) {
					require_next_checkmultisig = false;
					for (unsigned int i = 0; i < maybe_keys_count; i++)
						pubkeys[pubkeys_count++] = maybe_keys[i];
					maybe_keys_count = 0;
				} else
					ERROREXIT("ERROR: Found possible public keys but are not using them as they are not followed immediately by [OP_N] OP_CHECK[MULTI]SIG[VERIFY]\n");
			} else if (maybe_keys_count > 0) {
				if (maybe_keys_count == 1 && (*readpos == 172 || *readpos == 173)) {
					pubkeys[pubkeys_count++] = maybe_keys[0];
					maybe_keys_count = 0;
				} else if (((unsigned int)*readpos) - 80 == maybe_keys_count)
					require_next_checkmultisig = true;
				else
					ERROREXIT("ERROR: Found possible public keys but are not using them as they are not followed immediately by [OP_N] OP_CHECK[MULTI]SIG[VERIFY]\n");
			} else if (*readpos >= 172 && *readpos <= 175)
				ERROREXIT("ERROR: Found OP_CHECK[MULTI]SIG[VERIFY] without pubkey(s) immediately preceeding it\n");
		}

		if (pushlen != -1)
			readpos = push_start + pushlen;
		else
			readpos++;
	}

	return pubkeys_count;
}

int main(int argc, char* argv[]) {
	char mode = 0; // 0x1 == address, 0x2 == privkey
	const char *redeem_script_hex = NULL, *p2sh_address = NULL, *ascii_contract = NULL, *priv_key_str = NULL, *nonce_hex = NULL, *fullcontract_hex = NULL;

	// ARGPARSE
	int i;
	while ((i = getopt(argc, argv, "gcr:f:d:p:a:n:ht?")) != -1)
		switch(i) {
		case 'g':
		case 'c':
			if (mode != 0)
				USAGEEXIT("May only specify one of -g, -c\n");
			mode = i == 'g' ? 0x1 : 0x2;
			break;
		case 'r':
			if (mode != 0x1 || redeem_script_hex)
				USAGEEXIT("-r only allowed once and in -g mode\n");
			redeem_script_hex = optarg;
			break;
		case 'p':
			if (mode != 0x2 || priv_key_str)
				USAGEEXIT("-p only allowed once and in -c mode\n");
			priv_key_str = optarg;
			break;
		case 'd':
			if (p2sh_address || ascii_contract)
				USAGEEXIT("Only one contract allowed\n");
			p2sh_address = optarg;
			break;
		case 'a':
			if (ascii_contract)
				USAGEEXIT("Only one contract allowed\n");
			ascii_contract = optarg;
			break;
		case 'n':
			if (nonce_hex)
				USAGEEXIT("Only one nonce allowed\n");
			nonce_hex = optarg;
			break;
		case 'f':
			if (fullcontract_hex || ascii_contract || p2sh_address || nonce_hex)
				USAGEEXIT("-f is mutually exclusive with -d, -a, -n\n");
			fullcontract_hex = optarg;
			break;
		case 't':
			maybe_set_testnet(1);
			break;
		case 'h':
		case '?':
			usage();
			exit(0);
		default:
			ERROREXIT("getopt malfunction?\n");
		}

	// ARGCHECK
	if (!p2sh_address && !ascii_contract && !fullcontract_hex)
		USAGEEXIT("No contract provided\n");
	if (mode == 0x1 && !redeem_script_hex)
		USAGEEXIT("No redeem script specified\n");
	if (mode == 0x2 && !nonce_hex && !fullcontract_hex)
		USAGEEXIT("No nonce specified\n");
	if (mode == 0x2 && !priv_key_str)
		USAGEEXIT("No private key specified\n");

	secp256k1_context_t *secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

	// GLOBALCONV
	unsigned char p2sh_bytes[20];
	const char* address_type = "TEXT";
	if (p2sh_address) {
		address_type = contract_str_to_bytes(p2sh_address, p2sh_bytes);
		if (!address_type)
			ERROREXIT("Contract Address (%s) is invalid\n", p2sh_address);
	}

	unsigned char nonce[16];
	if (nonce_hex && !hex_to_bytes(nonce_hex, nonce, 16))
		ERROREXIT("Nonce is not a valid 16-byte hex string\n");

	if (fullcontract_hex) {
		unsigned char fullcontract[40];
		if (!hex_to_bytes(fullcontract_hex, fullcontract, 40))
			ERROREXIT("Full contract is not a valid 40-byte hex string\n");
		if (memcmp(fullcontract, "P2SH", 4) == 0)
			address_type = "P2SH";
		else if (memcmp(fullcontract, "P2PH", 4) == 0)
			address_type = "P2SH";
		else
			ERROREXIT("Invalid fullcontract type");

		memcpy(nonce, fullcontract + 4, sizeof(nonce));
		nonce_hex = "42"; // To make logic below work out

		memcpy(p2sh_bytes, fullcontract + 4 + sizeof(nonce), sizeof(p2sh_bytes));
	}

	maybe_set_testnet(0);

	// DOIT
	if (mode == 0x1) {
		unsigned int redeem_script_len = strlen(redeem_script_hex)/2;
		unsigned char redeem_script[redeem_script_len];
		if (!hex_to_bytes(redeem_script_hex, redeem_script, redeem_script_len))
			ERROREXIT("Invalid redeem script\n");

		unsigned char* keys[redeem_script_len / 33];
		int key_count = get_pubkeys_from_redeemscript(redeem_script, redeem_script_len, keys);
		if (key_count < 1)
			ERROREXIT("Redeem script invalid or no pubkeys found\n");

		FILE* rand;
		if (!nonce_hex) {
			rand = fopen("/dev/urandom", "rb");
			assert(rand);
		}

		unsigned char data[4 + 16 + (ascii_contract ? strlen(ascii_contract) : 20)];
		memset(data,                         0,              4);
		memcpy(data,                         address_type,   strlen(address_type));
		if (ascii_contract)
			memcpy(data + 4 + sizeof(nonce), ascii_contract, strlen(ascii_contract));
		else
			memcpy(data + 4 + sizeof(nonce), p2sh_bytes,     sizeof(p2sh_bytes));

		unsigned char keys_work[key_count][33];
		while (true) {
			for (i = 0; i < key_count; i++)
				memcpy(keys_work[i], keys[i], 33);

			if (!nonce_hex)
				assert(fread((char*)nonce, 1, 16, rand) == 16);
			memcpy(data + 4,                     nonce,          sizeof(nonce));

			for (i = 0; i < key_count; i++) {
				unsigned char res[32];
				hmac_sha256(res, keys_work[i], data, 4 + 16 + (ascii_contract ? strlen(ascii_contract) : 20));
				secp256k1_pubkey_t pubkey;
				if (!secp256k1_ec_pubkey_parse(secp256k1_ctx, &pubkey, keys_work[i], 33))
					ERROREXIT("INVALID PUBLIC KEY IN SCRIPT");
				if (secp256k1_ec_pubkey_tweak_add(secp256k1_ctx, &pubkey, res) == 0) {
					if (nonce_hex)
						ERROREXIT("YOU BROKE SHA256, PLEASE SEND THE EXACT DATA USED IN A BUG REPORT\n");
					break; // if tweak > order
				}
				int len = 33;
				secp256k1_ec_pubkey_serialize(secp256k1_ctx, keys_work[i], &len, &pubkey, 1);
				assert(len == 33);
			}
			if (i == key_count)
				break;
		}
		for (i = 0; i < key_count; i++)
			memcpy(keys[i], keys_work[i], 33);

		if (!nonce_hex)
			fclose(rand);

		char p2sh_res[35];
		p2sh_res[34] = 0;
		redeemscript_to_p2sh(p2sh_res, redeem_script, redeem_script_len);

		printf("Nonce: ");
		for (int i = 0; i < 16; i++)
			printf("%02x", nonce[i]);
		printf("\nFull serialized contract: ");
		for (unsigned int i =0 ; i < 4 + 16 + (ascii_contract ? strlen(ascii_contract) : 20); i++)
			printf("%02x", data[i]);
		printf("\nModified redeem script: ");
		for (unsigned int i = 0; i < redeem_script_len; i++)
			printf("%02x", redeem_script[i]);
		printf("\nModified redeem script as P2SH address: %s\n", p2sh_res);
	} else if (mode == 0x2) {
		unsigned char priv[33], pub[33];
		secp256k1_pubkey_t pubkey;
		if (!privkey_str_to_bytes(priv_key_str, priv))
			ERROREXIT("Private key is invalid (or not used as compressed)\n");

		unsigned char data[4 + 16 + (ascii_contract ? strlen(ascii_contract) : 20)];
		memset(data,                         0,              4);
		memcpy(data,                         address_type,   strlen(address_type));
		memcpy(data + 4,                     nonce,          sizeof(nonce));
		if (ascii_contract)
		    memcpy(data + 4 + sizeof(nonce), ascii_contract, strlen(ascii_contract));
		else
		    memcpy(data + 4 + sizeof(nonce), p2sh_bytes,     sizeof(p2sh_bytes));

		int len = 0;
		if (secp256k1_ec_pubkey_create(secp256k1_ctx, &pubkey, priv) != 1)
			ERROREXIT("Private key was invalid\n");
		secp256k1_ec_pubkey_serialize(secp256k1_ctx, pub, &len, &pubkey, 1);
		assert(len == 33);

		unsigned char tweak[32];
		hmac_sha256(tweak, pub, data, 4 + 16 + (ascii_contract ? strlen(ascii_contract) : 20));

		if (secp256k1_ec_privkey_tweak_add(secp256k1_ctx, priv, tweak) != 1)
			ERROREXIT("Tweak is invalid\n");

		priv[32] = 1;
		char res[52];
		bytes_to_privkey_str(priv, res);
		printf("New secret key: %s\n", res);
	} else
		ERROREXIT("OH GOD WHAT DID YOU DO?\n");

	secp256k1_context_destroy(secp256k1_ctx);

	return 0;
}
