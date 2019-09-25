#include "keystore.h"
#include <stdio.h>
#include "wally_crypto.h"
#include "wally_bip32.h"
#include "wally_bip39.h"
#include "wally_address.h"
#include "wally_script.h"
#include "networks.h"
#include "utility/ccan/ccan/endian/endian.h"

static uint32_t * parse_derivation(const char * path, size_t * derlen){
    static const char VALID_CHARS[] = "0123456789/'h";
    size_t len = strlen(path);
    const char * cur = path;
    if(path[0] == 'm'){ // remove leading "m/"
        cur+=2;
        len-=2;
    }
    if(cur[len-1] == '/'){ // remove trailing "/"
        len--;
    }
    size_t derivationLen = 1;
    // checking if all chars are valid and counting derivation length
    for(size_t i=0; i<len; i++){
        const char * pch = strchr(VALID_CHARS, cur[i]);
        if(pch == NULL){ // wrong character
            return NULL;
        }
        if(cur[i] == '/'){
            derivationLen++;
        }
    }
    uint32_t * derivation = (uint32_t *)calloc(derivationLen, sizeof(uint32_t));
    size_t current = 0;
    for(size_t i=0; i<len; i++){
        if(cur[i] == '/'){ // next
            current++;
            continue;
        }
        const char * pch = strchr(VALID_CHARS, cur[i]);
        uint32_t val = pch-VALID_CHARS;
        if(derivation[current] >= BIP32_INITIAL_HARDENED_CHILD){ // can't have anything after hardened
            free(derivation);
            return NULL;
        }
        if(val < 10){
            derivation[current] = derivation[current]*10 + val;
        }else{ // h or ' -> hardened
            derivation[current] += BIP32_INITIAL_HARDENED_CHILD;
        }
    }
    *derlen = derivationLen;
    return derivation;
}

int keystore_init(const char * mnemonic, const char * password, keystore_t * key){
    if(key == NULL){
        return -1;
    }
    if(mnemonic == NULL){
        key->root = NULL;
        return 0;
    }
    if(key->root != NULL){
        bip32_key_free(key->root);
        key->root = NULL;
    }
    int res;
    size_t len;
    uint8_t seed[BIP39_SEED_LEN_512];
    // FIXME: process results - something might go wrong
    res = bip39_mnemonic_to_seed(mnemonic, password, seed, sizeof(seed), &len);
    res = bip32_key_from_seed_alloc(seed, sizeof(seed), BIP32_VER_TEST_PRIVATE, 0, &key->root);
    wally_bzero(seed, sizeof(seed));
    uint8_t h160[20];
    wally_hash160(key->root->pub_key, sizeof(key->root->pub_key),
                  h160, sizeof(h160));
    for(int i=0; i<4; i++){
        sprintf(key->fingerprint+2*i, "%02x", h160[i]);
    }
    return 0;
}

int keystore_get_xpub(const keystore_t * key, const char * path, const network_t * network, int use_slip132, char ** xpub){
    struct ext_key * child = NULL;
    int res;
    size_t len = 0;
    uint32_t * derivation = parse_derivation(path, &len);
    if(derivation == NULL){
        return -1;
    }
    res = bip32_key_from_parent_path_alloc(key->root, derivation, len, BIP32_FLAG_KEY_PRIVATE, &child);
    child->version = network->xprv;
    uint8_t xpub_raw[BIP32_SERIALIZED_LEN];
    res = bip32_key_serialize(child, BIP32_FLAG_KEY_PUBLIC, xpub_raw, sizeof(xpub_raw));
    uint32_t ver = cpu_to_be32(network->xpub);
    if((len > 0) & use_slip132){
        switch(derivation[0]){
            case BIP32_INITIAL_HARDENED_CHILD+84:
            {
                ver = cpu_to_be32(network->zpub);
                break;
            }
            case BIP32_INITIAL_HARDENED_CHILD+49:
            {
                ver = cpu_to_be32(network->ypub);
                break;
            }
            case BIP32_INITIAL_HARDENED_CHILD+48:
            {
                if(len >= 4){
                    switch(derivation[3]){
                        case BIP32_INITIAL_HARDENED_CHILD+1:
                        {
                            ver = cpu_to_be32(network->Ypub);
                            break;
                        }
                        case BIP32_INITIAL_HARDENED_CHILD+2:
                        {
                            ver = cpu_to_be32(network->Zpub);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    memcpy(xpub_raw, &ver, 4);
    res = wally_base58_from_bytes(xpub_raw, sizeof(xpub_raw), BASE58_FLAG_CHECKSUM, xpub);
    bip32_key_free(child);
    free(derivation);
    return 0;
}

int keystore_get_addr(const keystore_t * key, const char * path, const network_t * network, char ** addr, int flag){
    struct ext_key * child = NULL;
    int res;
    size_t len = 0;
    uint32_t * derivation = parse_derivation(path, &len);
    if(derivation == NULL){
        return -1;
    }
    res = bip32_key_from_parent_path_alloc(key->root, derivation, len, BIP32_FLAG_KEY_PRIVATE, &child);
    child->version = network->xprv;

    if(flag == KEYSTORE_BECH32_ADDRESS){
        res |= wally_bip32_key_to_addr_segwit(child, network->bech32, 0, addr);
    }else{
        res |= wally_bip32_key_to_address(child, WALLY_ADDRESS_TYPE_P2SH_P2WPKH, network->p2sh, addr);
    }
    bip32_key_free(child);
    free(derivation);
    if(res!=WALLY_OK){
        wally_free_string(*addr);
    }
    return res;
}

int keystore_check_psbt(const keystore_t * key, const struct wally_psbt * psbt){
    // check inputs: at least one to sign
    uint8_t err = KEYSTORE_PSBTERR_CANNOT_SIGN;
    uint8_t h160[20];
    wally_hash160(key->root->pub_key, sizeof(key->root->pub_key),
                  h160, sizeof(h160));
    for(int i=0; i<psbt->num_inputs; i++){
        // check fingerprints in derivations
        if(psbt->inputs[i].keypaths == NULL){
            return KEYSTORE_PSBTERR_CANNOT_SIGN;
        }
        uint8_t can_sign = 0;
        for(int j=0; j<psbt->inputs[i].keypaths->num_items; j++){
            if(memcmp(psbt->inputs[i].keypaths->items[j].origin.fingerprint, h160, 4)==0){
                can_sign = 1;
                break;
            }
        }
        if(can_sign){
            err = 0;
        }else{
            if(err == 0){ // if can't sign but could sign previous
                return KEYSTORE_PSBTERR_MIXED_INPUTS;
            }
        }
        // TODO: add verification that all inputs correspond to the same policy
        // just forcing single key for now
        if(psbt->inputs[i].keypaths->num_items != 1){
            return KEYSTORE_PSBTERR_UNSUPPORTED_POLICY;
        }
    }
    // TODO: check all fields in the psbt
    return err;
}

int keystore_output_is_change(const keystore_t * key, const struct wally_psbt * psbt, uint8_t i, char ** warning){
    // TODO: check if it is a change
    if(i >= psbt->num_outputs){
        return 0;
    }
    if(psbt->outputs[i].keypaths == NULL){
        return 0;
    }
    if(psbt->outputs[i].keypaths->num_items != 1){
        printf("keystore: multisig change detection is not supported yet\r\n");
        return 0;
    }
    struct ext_key * pk = NULL;
    // TODO: fix for multiple keypaths
    bip32_key_from_parent_path_alloc(key->root, 
        psbt->outputs[i].keypaths->items[0].origin.path, 
        psbt->outputs[i].keypaths->items[0].origin.path_len, 
        BIP32_FLAG_KEY_PRIVATE, &pk);
    size_t script_type;
    wally_scriptpubkey_get_type(psbt->tx->outputs[i].script, psbt->tx->outputs[i].script_len, &script_type);
    // should deal with all script types, only P2WPKH for now

    // doesn't matter, we just compare strings...
    // TODO: refactor with scriptpubkey instead of addresses
    const network_t * network = &Mainnet;
    char * addr = NULL;
    char * addr2 = NULL;
    uint8_t bytes[21];
    switch(script_type){
        case WALLY_SCRIPT_TYPE_P2WPKH:
            wally_addr_segwit_from_bytes(psbt->tx->outputs[i].script, psbt->tx->outputs[i].script_len, network->bech32, 0, &addr);
            wally_bip32_key_to_addr_segwit(pk, network->bech32, 0, &addr2);
            break;
        case WALLY_SCRIPT_TYPE_P2SH:
            bytes[0] = network->p2sh;
            memcpy(bytes+1, psbt->tx->outputs[i].script+2, 20);
            wally_base58_from_bytes(bytes, 21, BASE58_FLAG_CHECKSUM, &addr);
            wally_bip32_key_to_address(pk, WALLY_ADDRESS_TYPE_P2SH_P2WPKH, network->p2sh, &addr2);
            break;
        case WALLY_SCRIPT_TYPE_P2PKH:
            bytes[0] = network->p2pkh;
            memcpy(bytes+1, psbt->tx->outputs[i].script+3, 20);
            wally_base58_from_bytes(bytes, 21, BASE58_FLAG_CHECKSUM, &addr);
            wally_bip32_key_to_address(pk, WALLY_ADDRESS_TYPE_P2PKH, network->p2pkh, &addr2);
            break;
        default:
            return 0;
    }
    int res = 0;
    if(strcmp(addr, addr2) == 0){
        res = 1;
    }
    bip32_key_free(pk);
    wally_free_string(addr2);
    wally_free_string(addr);
    return res;
}

int keystore_sign_psbt(const keystore_t * key, struct wally_psbt * psbt, char ** output){
    size_t len;
    struct wally_psbt * signed_psbt;
    wally_psbt_init_alloc(
        psbt->num_inputs,
        psbt->num_outputs,
        0,
        &signed_psbt);
    wally_psbt_set_global_tx(psbt->tx, signed_psbt);
    for(int i = 0; i < psbt->num_inputs; i++){
        if(!psbt->inputs[i].witness_utxo){
            return -1;
        }
        uint8_t hash[32];
        uint8_t script[25];

        struct ext_key * pk = NULL;
        // TODO: fix for multiple keypaths
        bip32_key_from_parent_path_alloc(key->root, 
            psbt->inputs[i].keypaths->items[0].origin.path, 
            psbt->inputs[i].keypaths->items[0].origin.path_len, 
            BIP32_FLAG_KEY_PRIVATE, &pk);

        wally_scriptpubkey_p2pkh_from_bytes(pk->pub_key, EC_PUBLIC_KEY_LEN, 
                WALLY_SCRIPT_HASH160, 
                script, 25, 
                &len);

        wally_tx_get_btc_signature_hash(psbt->tx, i, 
                script, len,
                psbt->inputs[i].witness_utxo->satoshi,
                WALLY_SIGHASH_ALL,
                WALLY_TX_FLAG_USE_WITNESS,
                hash, 32
            );
 
        uint8_t sig[EC_SIGNATURE_LEN];
        wally_ec_sig_from_bytes(
                pk->priv_key+1, 32, // first byte of ext_key.priv_key is 0x00
                hash, 32,
                EC_FLAG_ECDSA,
                sig, EC_SIGNATURE_LEN
            );
        uint8_t der[EC_SIGNATURE_DER_MAX_LEN+1];
        wally_ec_sig_to_der(
                sig, EC_SIGNATURE_LEN,
                der, EC_SIGNATURE_DER_MAX_LEN,
                &len
            );
        der[len] = WALLY_SIGHASH_ALL;
        if(!signed_psbt->inputs[i].partial_sigs){
            partial_sigs_map_init_alloc(1, &signed_psbt->inputs[i].partial_sigs);
        }
        add_new_partial_sig(signed_psbt->inputs[i].partial_sigs,
                pk->pub_key, 
                der, len+1
            );
        bip32_key_free(pk);
    }
 
    wally_psbt_to_base64(signed_psbt, output);
    wally_psbt_free(signed_psbt);
    return 0;
}


// maybe expose it to public interface
static int keystore_get_wallets_number(const keystore_t * key, const network_t * network){
    char path[100];
    sprintf(path, "/internal/%s", key->fingerprint);
    storage_maybe_mkdir(path);
    sprintf(path, "/internal/%s/%s", key->fingerprint, network->name);
    storage_maybe_mkdir(path);
                              // folder, extension
    return storage_get_file_count(path, ".wallet");
}

static int keystore_get_wallet_name(const keystore_t * key, const network_t * network, int i, char ** wname){
    char path[100];
    sprintf(path, "/internal/%s/%s/%d.wallet", key->fingerprint, network->name, i);
    FILE *f = fopen(path, "r");
    if(!f){
        return -1;
    }
    char w[100] = "Undefined";
    int m; int n;
    fscanf(f, "name=%[^\n]\ntype=%*s\nm=%d\nn=%d", w, &m, &n);
    sprintf(w+strlen(w), " (%d of %d)", m, n);
    *wname = (char *)malloc(strlen(w)+1);
    strcpy(*wname, w);
    return strlen(wname);
}

int keystore_get_wallets(const keystore_t * key, const network_t * network, char *** wallets){
    int num_wallets = keystore_get_wallets_number(key, network);
    if(num_wallets < 0){ // error
        return num_wallets;
    }
    char ** w = (char **)calloc(num_wallets+2, sizeof(char *));
    // first - default single key wallet
    w[0] = (char *)calloc(30, sizeof(char));
    sprintf(w[0], "Default (single key)");
    // multisig wallets:
    for(int i=1; i<num_wallets+1; i++){
        keystore_get_wallet_name(key, network, i-1, &w[i]);
    }
    // last - empty string
    w[num_wallets+1] = (char *)calloc(1, sizeof(char));
    *wallets = w;
    return num_wallets;
}

int keystore_free_wallets(char ** wallets){
    int i=0;
    if(wallets == NULL){
        return -1;
    }
    while(wallets[i] != NULL && strlen(wallets[i]) > 0){ // free everything that is not empty
        free(wallets[i]);
        i++;
    }
    free(wallets[i]); // last one
    free(wallets); // free the list
    wallets = NULL;
    return 0;
}

int keystore_get_wallet(const keystore_t * key, const network_t * network, int val, wallet_t * wallet){
    wallet->val = val;
    wallet->keystore = key;
    wallet->network = network;
    wallet->address = 0;
    if(val == 0){
        sprintf(wallet->name, "Default (single key)");
    }else{
        char * wname;
        keystore_get_wallet_name(key, network, val-1, &wname);
        strcpy(wallet->name, wname);
        free(wname);
    }
    return 0;
}

int wallet_get_addresses(const wallet_t * wallet, char ** base58_addr, char ** bech32_addr){
    if(wallet->val == 0){ // single key
        char path[50];
        sprintf(path, "m/84h/%dh/0h/0/%d", wallet->network->bip32, wallet->address);
        keystore_get_addr(wallet->keystore, path, wallet->network, bech32_addr, KEYSTORE_BECH32_ADDRESS);
        keystore_get_addr(wallet->keystore, path, wallet->network, base58_addr, KEYSTORE_BASE58_ADDRESS);
    }else{ // TODO: incapsulate, i.e. to storage
        char path[100];
        sprintf(path, "/internal/%s/%s/%d.wallet", wallet->keystore->fingerprint, wallet->network->name, wallet->val-1);
        FILE *f = fopen(path, "r");
        if(!f){
            printf("missing file\r\n");
            return -1;
        }
        int m; int n;
        fscanf(f, "name=%*[^\n]\ntype=%*s\nm=%d\nn=%d\n", &m, &n);
        char xpub[150];
        uint8_t * pubs = (uint8_t *)malloc(33*n);
        for(int i=0; i<n; i++){
            fscanf(f, "[%*[^]]]%s\n", xpub);
            struct ext_key * k;
            int res = bip32_key_from_base58_alloc(xpub, &k);
            if(res != 0){
                free(pubs);
                printf("cant parse key: %s\r\n", xpub);
                return -1;
            }
            struct ext_key * k2;
            // from path doesn't work - requires fixing libwally
            bip32_key_from_parent_alloc(k, 0, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &k2);
            bip32_key_from_parent(k2, wallet->address, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, k);
            memcpy(pubs+33*i, k->pub_key, 33);
            bip32_key_free(k);
            bip32_key_free(k2);
        }
        size_t len = 34*n+3;
        uint8_t * script = (uint8_t *)malloc(len);
        size_t lenout = 0;
        wally_scriptpubkey_multisig_from_bytes(pubs, 33*n, m, 0, script, len, &lenout);
        free(pubs);
        
        uint8_t hash[34];
        hash[0] = 0;
        hash[1] = 32;
        wally_sha256(script, lenout, hash+2, 32);
        free(script);
        wally_addr_segwit_from_bytes(hash, sizeof(hash), wallet->network->bech32, 0, bech32_addr);
        uint8_t bytes[21];
        bytes[0] = wallet->network->p2sh;
        wally_hash160(hash, sizeof(hash), bytes+1, 20);
        wally_base58_from_bytes(bytes, 21, BASE58_FLAG_CHECKSUM, base58_addr);
    }
    return 0;
}

int keystore_check_wallet(const keystore_t * keystore, const network_t * network, const char * buf){
    // FIXME: hardcoded lengths are bad...
    char name[100];
    char type[10];
    int m;
    int n;
    char * rest;
    rest = (char *)calloc(strlen(buf)+1, 1);
    int res = sscanf(buf, "name=%[^\n]\ntype=%[^\n]\nm=%d\nn=%d\n%[^\0]", name, type, &m, &n, rest);
    if(res < 5){
        return KEYSTORE_WALLET_ERR_PARSING;
    }
    char derivation[150];
    char xpub[150];
    char * line = rest;
    int err = KEYSTORE_WALLET_ERR_NOT_INCLUDED;
    for(int i=0; i<n; i++){
        res = sscanf(line, "[%[^]]]%s", derivation, xpub);
        printf("%s - %s\r\n", derivation, xpub);
        if(res < 2){
            line = NULL;
            free(rest);
            return KEYSTORE_WALLET_ERR_PARSING;
        }
        if(memcmp(derivation, keystore->fingerprint, 8) == 0){
            char * mypub;
            char * myslippub;
            res = keystore_get_xpub(keystore, derivation+9, network, 0, &mypub);
            res = keystore_get_xpub(keystore, derivation+9, network, 0, &myslippub);
            if(res < 0){
                free(rest);
                return KEYSTORE_WALLET_ERR_WRONG_XPUB;
            }
            if((strcmp(mypub, xpub) == 0) || (strcmp(myslippub, xpub)==0)){
                wally_free_string(mypub);
                wally_free_string(myslippub);
                err = 0;
            }else{
                wally_free_string(mypub);
                wally_free_string(myslippub);
                free(rest);
                return KEYSTORE_WALLET_ERR_WRONG_XPUB;
            }
        }
        line += strlen(derivation)+strlen(xpub)+3;
    }
    free(rest);
    return err;
}

int keystore_add_wallet(const keystore_t * keystore, const network_t * network, const char * buf, wallet_t * wallet){
    char path[100];
    sprintf(path, "/internal/%s", keystore->fingerprint);
    storage_maybe_mkdir(path);
    sprintf(path, "/internal/%s/%s", keystore->fingerprint, network->name);
    storage_maybe_mkdir(path);
                // folder, data, extension
    return storage_push(path, buf, ".wallet");
}