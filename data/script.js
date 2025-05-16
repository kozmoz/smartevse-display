
const TEN_SECONDS_IN_MS = 10000;

const ELM_UL_NETWORK_LIST = document.getElementById('networkList');
const ELM_DIV_NETWORK_DETAILS = document.getElementById('networkDetails');
const ELM_H2_SELECTED_SSID = document.getElementById('selectedSSID');
const ELM_FORM_NETWORK = document.getElementById('networkForm');
const ELM_INPUT_NETWORK_SSID = document.getElementById('networkSSID');
const ELM_INPUT_NETWORK_PASSWORD = document.getElementById('networkPassword');
const ELM_DIV_SPINNER = document.getElementById('spinner');
const ELM_DIV_ERROR_MESSAGE = document.getElementById('error');

// Form validation.
ELM_FORM_NETWORK.onsubmit = (event) => {
    if (ELM_INPUT_NETWORK_PASSWORD.value.length === 0) {
        ELM_DIV_ERROR_MESSAGE.textContent = "Password is a required field.";
        ELM_DIV_ERROR_MESSAGE.style.display = 'block';
        event.preventDefault();
        return;
    }
    if (ELM_INPUT_NETWORK_SSID.value.length === 0) {
        ELM_DIV_ERROR_MESSAGE.textContent = "No network selected.";
        ELM_DIV_ERROR_MESSAGE.style.display = 'block';
        event.preventDefault();
    }
}

let selectedNetwork = null;
let abortController = new AbortController();

function loadNetworks() {

    // Cancel previous request
    abortController.abort();
    abortController = new AbortController();

    ELM_DIV_SPINNER.style.display = 'block';
    fetch('/api/wifi', {
        signal: abortController.signal
    })
        .then(response => response.json())
        .then(data => {
            ELM_DIV_SPINNER.style.display = 'none';
            ELM_DIV_ERROR_MESSAGE.style.display = 'none';
            ELM_UL_NETWORK_LIST.innerHTML = '';
            data.forEach(network => {
                const listItem = document.createElement('li');
                listItem.textContent = `${network.ssid} ${network.open ? '(Open)' : ''} - rssi ${network.rssi}`;
                listItem.onclick = () => selectNetwork(network);
                ELM_UL_NETWORK_LIST.appendChild(listItem);
            });
        })
        .catch((err) => {
            if (err.name !== 'AbortError') {
                ELM_DIV_SPINNER.style.display = 'none';
                ELM_DIV_ERROR_MESSAGE.textContent = "Failed to load networks.";
                ELM_DIV_ERROR_MESSAGE.style.display = 'block';
            }
        });
}

function selectNetwork(network) {
    selectedNetwork = network;
    ELM_H2_SELECTED_SSID.textContent = network.ssid;
    ELM_INPUT_NETWORK_SSID.value = network.ssid;
    ELM_INPUT_NETWORK_PASSWORD.value = '';
    ELM_DIV_NETWORK_DETAILS.classList.remove('hidden');
    ELM_DIV_NETWORK_DETAILS.classList.add('active');
    ELM_UL_NETWORK_LIST.classList.add('hidden');
}

function cancelSelection() {
    selectedNetwork = null;
    ELM_DIV_NETWORK_DETAILS.classList.add('hidden');
    ELM_UL_NETWORK_LIST.classList.remove('hidden');
}

loadNetworks();
setInterval(loadNetworks, TEN_SECONDS_IN_MS);
