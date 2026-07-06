import { render } from "preact";
import { App } from "./app.tsx";

const root = document.getElementById("app");
if (root) render(<App />, root);
