
<script>
    import {onMount} from "svelte";

    let fetchPromise = (async function() {return await (await fetch("/scripts")).json()})();
    let scripts = {};
    onMount(async () => {
        scripts = await fetchPromise;
    })
    async function activate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/activate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
    }

    async function deactivate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/deactivate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
    }
    function deleteScript(script) {
        if(!confirm("Are you sure you want to delete " + script + "?")) {
            return;
        }
        delete scripts[script]
        scripts = scripts
    }
</script>

<main>
    {#await fetchPromise}
    {:then}
        <div id="scripts">
        {#each Object.keys(scripts) as script (script)}
            <div class="script">
                <div class="scriptName">
                    {scripts[script].name}
                </div>
                {#if scripts[script].active}
                    <button on:click={deactivate(script)}>
                        Deactivate
                    </button>
                {:else}
                    <button on:click={activate(script)}>
                        Activate
                    </button>
                {/if}
                <button on:click={deleteScript(script)}>
                    Delete
                </button>
            </div>
        {/each}
        </div>
    {:catch error}
        <div id="scripts">
            {error.message}
        </div>
    {/await}
</main>

<style>
.script {
    background-color: white;
    border-radius: 10px;
    padding: 10px;
    box-shadow: #9f9f9f 0 0 5px;
    margin-bottom: 20px;
    font-size: var(--medium-font-size);
    display: grid;
    grid-template-columns: auto min-content min-content;
    align-items: center;
}
.script > button {
    margin: 0 0 0 10px;
}
</style>