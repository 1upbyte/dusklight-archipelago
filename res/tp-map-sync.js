// Loaded only by the mod's local copy of tp-map. Its existing Flag.setMarker()/unsetMarker()
// paths continue to own the storage, tracker, requirement, and visual updates.
(() => {
    const key = 'dusklightLiveChecks';
    let previous;
    try {
        previous = JSON.parse(localStorage.getItem(key)) || {seed: '', names: []};
    } catch (_) {
        previous = {seed: '', names: []};
    }
    if (!Array.isArray(previous.names)) previous.names = [];

    const status = document.createElement('div');
    status.id = 'dusklightLiveStatus';
    status.style.cssText = 'font-size:12px;margin-top:5px;opacity:.8';
    status.textContent = 'Dusklight: connecting…';
    document.getElementById('credit')?.appendChild(status);

    function resolve(name) {
        let flag = flags.get(name) || AlternateDusklightFlagNames.get(name);
        if (!flag && name.endsWith(' Hint Sign')) {
            const sign = name.replace(' Hint Sign', ' Sign');
            flag = flags.get(sign) || AlternateDusklightFlagNames.get(sign);
        }
        return flag;
    }

    async function sync() {
        try {
            const response = await fetch('/api/checks', {cache: 'no-store'});
            if (!response.ok) throw new Error(`HTTP ${response.status}`);
            const data = await response.json();
            if (typeof data.seed !== 'string' || !Array.isArray(data.checks))
                throw new Error('Invalid check snapshot');

            const desired = new Set();
            for (const name of data.checks) {
                if (typeof name !== 'string') continue;
                const flag = resolve(name);
                if (flag) desired.add(flag.name);
            }

            const auto = new Set(previous.names);
            if (previous.seed !== data.seed) {
                for (const name of auto) {
                    const flag = flags.get(name);
                    if (flag?.isSet()) flag.unsetMarker();
                }
                auto.clear();
            } else {
                for (const name of auto) {
                    if (!desired.has(name)) {
                        const flag = flags.get(name);
                        if (flag?.isSet()) flag.unsetMarker();
                        auto.delete(name);
                    }
                }
            }

            for (const name of desired) {
                const flag = flags.get(name);
                if (flag && !flag.isSet()) {
                    flag.setMarker();
                    auto.add(name);
                }
            }
            previous = {seed: data.seed, names: [...auto]};
            localStorage.setItem(key, JSON.stringify(previous));
            status.textContent = data.seed
                ? `Dusklight: ${desired.size} map checks synced`
                : 'Dusklight: waiting for a save';
        } catch (error) {
            status.textContent = 'Dusklight: connection lost';
            console.warn('Dusklight check sync:', error);
        }
    }

    sync();
    setInterval(sync, 1000);
})();
